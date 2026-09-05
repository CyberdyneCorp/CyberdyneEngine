// Golden images: the milestone's artefact, photographed. Task 6.3, and task 7.1's first half.
//
// ================================================================================================
// WHAT IS PHOTOGRAPHED IS THE SAMPLE, NOT A COPY OF IT
// ================================================================================================
//
// The frame these cases render is `samples/03-first-light`'s, reached through `cy::sample-first-
// light` — the library half of the milestone's artefact. A golden suite with a scene of its own
// would drift from the sample within a milestone, and the references would then be photographs of
// something nobody ships. This way, a change that breaks the artefact turns a gate red.
//
// ================================================================================================
// THREE CASES, AND WHY EACH IS NOT THE OTHER TWO
// ================================================================================================
//
//   the scene              the reference the milestone closes on.
//   the scene at 1e6       THE SAME REFERENCE FILE. design.md §3: camera-relative rendering lands
//                          with the first draw, and the claim is not "it still looks right" but
//                          "it is the same image". A second reference would let the two drift.
//   the sun's shadow off   a DIFFERENT reference, plus the assertion that the two references
//                          differ. Without it, a frame that had quietly stopped sampling the shadow
//                          map would keep passing the first case forever — the golden image would
//                          be regenerated along with the defect.
//
// The fourth case is not a golden image at all: the same frame through the null backend, structure
// for structure. See test_null_frame.cpp for the rest of that argument.
//
// ================================================================================================
// THE TOLERANCE
// ================================================================================================
//
// golden.h derives it; it is not a number anyone tuned. Two texels of channel difference is one
// quantisation step of the 8-bit target plus one step of headroom, and a texel may only differ at
// all where the reference itself has a high-contrast neighbour — a minified checkerboard's texels
// can flip between two greys on the last bit of a UV, and nothing else in the frame can.
//
// ================================================================================================
// REGENERATING A REFERENCE
// ================================================================================================
//
// `CY_RENDER_UPDATE_GOLDEN=1 ctest -R render.golden` writes each reference and then FAILS, naming
// what it wrote. Failing is the point: `testing-and-quality` requires regeneration to be "a
// deliberate, reviewed step", and a mode that regenerated and passed would let a run with the
// variable set in the environment launder a defect into the repository.

#include <cy/test/test.h>

#include <cy/backends/rhi/device.h>

#include "device.h"
#include "golden.h"
#include "renderer.h"
#include "scene.h"

#include <cstdio>
#include <cstdlib>

namespace {

using cy::render_test::DeviceFixture;
using cy::sample::first_light::Camera;
using cy::sample::first_light::FrameReport;
using cy::sample::first_light::Renderer;
using cy::sample::first_light::RendererOptions;
using cy::sample::first_light::Scene;
using cy::sample::first_light::SceneDescription;

/// The golden viewport. Small on purpose: a reference is a committed binary and this encoder does
/// not compress, so 192x108 costs about 62 KiB a file. It is still large enough that the pillar,
/// six boxes, their shadows and the checkerboard's minification are all in the picture, which is
/// what the reference has to be able to fail on.
constexpr cy::u32 kWidth = 192;
constexpr cy::u32 kHeight = 108;

const char* reference_path(const char* name) noexcept {
    static char storage[1024];
    std::snprintf(storage, sizeof(storage), "%s/references/%s.png", CY_RENDER_TEST_DIR, name);
    return storage;
}

bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/// Render the sample's frame once, at the first phase of its orbit, and hand back the colour
/// target.
///
/// Phase 0 rather than a phase somebody liked: it is the only phase a reader can reproduce from the
/// scene alone, and `--frames 1` on the sample is the same frame.
cy::Status render_scene(DeviceFixture& fixture, const SceneDescription& description,
                        cy::render_test::Image& out, FrameReport& report) {
    Scene scene(fixture.allocator());
    if (cy::Status built = scene.build(description); !built) {
        return built;
    }
    RendererOptions options;
    options.width = kWidth;
    options.height = kHeight;

    Renderer renderer(fixture.allocator(), fixture.device());
    if (cy::Status prepared = renderer.prepare(scene, options); !prepared) {
        return prepared;
    }
    const Camera camera = scene.camera_at(0.0F);
    cy::Expected<FrameReport, cy::Error> frame = renderer.render(scene, camera);
    if (!frame.has_value()) {
        return cy::make_unexpected(frame.error());
    }
    report = *frame;
    return cy::render_test::adopt(out, renderer.color_texels(), kWidth, kHeight);
}

/// Compare against a committed reference, or write it when the run was asked to. Returns false when
/// a reference was written, which the case turns into a failure — see the header comment.
bool check_against_reference(const cy::render_test::Image& rendered, const char* name) {
    const char* path = reference_path(name);
    if (updating_references()) {
        const cy::Status written = cy::render_test::write_png(path, rendered);
        CY_CHECK(written.has_value());
        std::fprintf(stderr,
                     "CY_RENDER_UPDATE_GOLDEN: wrote %s. Look at it, then commit it — this run "
                     "fails on purpose so that a regenerating run can never be a passing one.\n",
                     path);
        // The failure IS the mechanism. `testing-and-quality` requires regeneration to be "a
        // deliberate, reviewed step", and a mode that regenerated and then passed would let a run
        // with this variable set in its environment launder a defect into the repository.
        CY_CHECK_FALSE(updating_references());
        return false;
    }

    cy::render_test::Image reference(rendered.texels.allocator());
    const cy::Status read = cy::render_test::read_png(path, reference);
    if (!read) {
        std::fprintf(stderr, "golden: %s: %s\n", path, read.error().message);
        CY_CHECK(read.has_value());
        return false;
    }

    const cy::render_test::Comparison comparison = cy::render_test::compare(reference, rendered);
    CY_CHECK(comparison.comparable);
    if (comparison.differing_off_edge != 0 || !comparison.comparable) {
        char diff_path[1024];
        std::snprintf(diff_path, sizeof(diff_path), "%s-difference.png", name);
        (void)cy::render_test::write_difference(diff_path, reference, rendered);
        std::fprintf(stderr,
                     "golden: %s differs — %u texels over tolerance (%u of them away from any "
                     "high-contrast edge), worst channel delta %u at (%u, %u). "
                     "The difference is at %s.\n",
                     name, comparison.differing, comparison.differing_off_edge,
                     comparison.max_channel_delta, comparison.worst_x, comparison.worst_y,
                     diff_path);
    }
    // THE ASSERTION. A texel may differ only where the reference has a high-contrast neighbour, and
    // only there because a minified checkerboard's sampled texel can flip on the last bit of a UV.
    // Anywhere else, a difference is a shading difference. See golden.h.
    CY_CHECK_EQ(comparison.differing_off_edge, 0U);
    CY_CHECK_LE(comparison.differing, comparison.edge_texels);
    // And the stronger claim, which is the one that holds on the machine the reference came from
    // and the one a regression breaks first.
    CY_CHECK_EQ(comparison.differing, 0U);
    return true;
}

}  // namespace

CY_TEST_CASE("render.golden: the lit, textured, shadowed scene matches its reference") {
    DeviceFixture fixture("vulkan", "cy_test_render_golden");
    if (!fixture.is(cy::rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }
    cy::render_test::Image rendered(fixture.allocator());
    FrameReport report{};
    const cy::Status ok = render_scene(fixture, SceneDescription{}, rendered, report);
    CY_REQUIRE(ok.has_value());

    (void)check_against_reference(rendered, "first_light");
    // `rhi-and-render-graph`: "a frame that renders but trips validation is not a frame that
    // works".
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
    // The frame the sample declares: an upload, a shadow pass, the forward pass, the readback and
    // the host boundary. Asserted here as well as in the null suite because a golden image that
    // matched while the frame had lost a pass would be a golden image of a coincidence.
    CY_CHECK_EQ(report.passes_recorded, 5U);
    CY_CHECK_EQ(report.passes_culled, 0U);
    CY_CHECK_EQ(report.submits, 1U);
    CY_CHECK_GT(report.barriers, 0U);
}

// ==================================================================================================
// THE REGRESSION CASE. Every other case here renders ONE frame, and that is exactly how the defect
// below survived every device suite in the milestone.
//
// `VulkanDevice::allocate_descriptor_set(layout, per_frame)` took the flag, recorded it, and then
// allocated from the CURRENT FRAME'S pool whichever value it had — and a frame pool is reset the
// moment its slot comes round. So a set the caller asked to be persistent was recycled after
// `frames_in_flight` frames, and from that frame on every draw bound a VkDescriptorSet the driver
// had already destroyed. The sample is the engine's only consumer of a persistent set today, and it
// hit this from frame 3: 24 validation errors a frame, on a run that still printed "exit 0
// (clean)".
//
// A suite that renders one frame cannot see it. This one renders past the ring twice over and
// asserts the frame is still the SAME IMAGE at the end — because a descriptor pointing at recycled
// memory can also be a frame that happens to look right, and "no validation errors" alone would not
// notice a set that had quietly stopped naming the shadow map.
// ==================================================================================================
CY_TEST_CASE("render.golden: the frame survives more frames than the device holds in flight") {
    DeviceFixture fixture("vulkan", "cy_test_render_golden");
    if (!fixture.is(cy::rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }

    Scene scene(fixture.allocator());
    CY_REQUIRE(scene.build(SceneDescription{}).has_value());
    RendererOptions options;
    options.width = kWidth;
    options.height = kHeight;
    Renderer renderer(fixture.allocator(), fixture.device());
    CY_REQUIRE(renderer.prepare(scene, options).has_value());

    // Twice round the ring and one more, so the pools have been recycled and re-recycled.
    const cy::u32 frames = (fixture.device().frames_in_flight() * 2U) + 1U;
    CY_CHECK_GT(frames, 2U);
    for (cy::u32 index = 0; index < frames; ++index) {
        // Phase 0 every time: the frame under test is the one the reference was taken of, so the
        // comparison at the end is against the committed image rather than against a second render.
        CY_REQUIRE(renderer.render(scene, scene.camera_at(0.0F)).has_value());
    }

    cy::render_test::Image rendered(fixture.allocator());
    CY_REQUIRE(
        cy::render_test::adopt(rendered, renderer.color_texels(), kWidth, kHeight).has_value());
    (void)check_against_reference(rendered, "first_light");
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("render.golden: the same scene a million units out is the same image") {
    DeviceFixture fixture("vulkan", "cy_test_render_golden");
    if (!fixture.is(cy::rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }
    SceneDescription description;
    // Tasks 5.3 and 7.5. An f32 world position has a spacing of about 0.06 units here, and the
    // scene's smallest feature is smaller than that — so a renderer that had narrowed before
    // subtracting would produce a visibly different picture, not a slightly different one.
    description.origin = 1000000.0;

    cy::render_test::Image rendered(fixture.allocator());
    FrameReport report{};
    const cy::Status ok = render_scene(fixture, description, rendered, report);
    CY_REQUIRE(ok.has_value());

    // THE SAME REFERENCE FILE as the case above. Not "close to"; the same one.
    (void)check_against_reference(rendered, "first_light");
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("render.golden: with the sun's shadow off the image is a different one") {
    DeviceFixture fixture("vulkan", "cy_test_render_golden");
    if (!fixture.is(cy::rhi::BackendKind::Vulkan)) {
        fixture.report_skip();
        return;
    }
    SceneDescription description;
    description.sun_shadows = false;

    cy::render_test::Image rendered(fixture.allocator());
    FrameReport report{};
    const cy::Status ok = render_scene(fixture, description, rendered, report);
    CY_REQUIRE(ok.has_value());
    (void)check_against_reference(rendered, "first_light_no_shadows");

    // AND THE CONTROL. Two references that happened to be identical would mean the shadow map was
    // sampled and thrown away, and both cases above would pass forever.
    cy::render_test::Image shadowed(fixture.allocator());
    const cy::Status read = cy::render_test::read_png(reference_path("first_light"), shadowed);
    if (read.has_value()) {
        const cy::render_test::Comparison against_shadowed =
            cy::render_test::compare(shadowed, rendered);
        CY_CHECK(against_shadowed.comparable);
        CY_CHECK_GT(against_shadowed.differing, 0U);
        std::fprintf(stderr, "golden: the shadow is worth %u texels of this frame\n",
                     against_shadowed.differing);
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("render.golden: the null backend derives the same frame the device does") {
    DeviceFixture vulkan("vulkan", "cy_test_render_golden");
    if (!vulkan.is(cy::rhi::BackendKind::Vulkan)) {
        vulkan.report_skip();
        return;
    }
    cy::render_test::Image on_device(vulkan.allocator());
    FrameReport device_report{};
    CY_REQUIRE(render_scene(vulkan, SceneDescription{}, on_device, device_report).has_value());

    DeviceFixture null("null", "cy_test_render_golden");
    CY_REQUIRE(null.is(cy::rhi::BackendKind::Null));
    cy::render_test::Image on_null(null.allocator());
    FrameReport null_report{};
    CY_REQUIRE(render_scene(null, SceneDescription{}, on_null, null_report).has_value());

    // STRUCTURE, NOT PLAN HASH. `CompiledGraph::plan_hash` covers every placement offset, and a
    // placement offset comes from the DEVICE's memory requirements — a real allocator's alignments
    // are not a synthetic one's, so the two hashes differ and should. What must agree is everything
    // the declarations decided: which passes survive, how many submits they fall into, how many
    // barriers the derivation emitted, and how many of those were ownership transfers.
    CY_CHECK_EQ(null_report.passes_recorded, device_report.passes_recorded);
    CY_CHECK_EQ(null_report.passes_culled, device_report.passes_culled);
    CY_CHECK_EQ(null_report.submits, device_report.submits);
    CY_CHECK_EQ(null_report.barriers, device_report.barriers);
    CY_CHECK_EQ(null_report.barrier_batches, device_report.barrier_batches);
    CY_CHECK_EQ(null_report.queue_ownership_transfers, device_report.queue_ownership_transfers);
    CY_CHECK_EQ(null_report.draws, device_report.draws);
    CY_CHECK_EQ(null_report.triangles, device_report.triangles);
    // And the null backend executes nothing, which is the honest half: it derives the frame, it
    // does not render it. Every texel it "read back" is whatever the readback buffer held.
    CY_CHECK_EQ(on_null.width, on_device.width);
}
