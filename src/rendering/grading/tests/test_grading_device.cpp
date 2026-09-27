// SPDX-License-Identifier: MIT
// Exposure and colour grading on a Vulkan device, with validation and synchronisation validation
// on. `render.grading`.
//
// ================================================================================================
// WHAT EACH CASE CAN FAIL ON
// ================================================================================================
//
// THE METERING AGAINST ITS HOST TWIN is test_metering_device.cpp, in this suite.
//
// THE FRAME. `pipeline_test::FrameScene` — the one scene the pipeline suites render — with the
// graded resolve as its post-process callback and the metering chain declared after its passes:
//
//   * auto-exposure converges to its target within the stated adaptation time
//     (`adaptation_seconds(speed, 0.05)`), follows every frame of the host's `adapt_ev100`, and
//     after a four-times brightness step and back, rises two stops at the brightening speed and
//     returns at the darkening one — and the resolve applies exactly the EV the frame before it
//     adapted to, byte for byte against the same EV pushed manually;
//   * a manual EV100 one stop higher halves every unclipped pixel, through a linear curve;
//   * an identity look and neutral controls draw the frame without grading byte for byte, and so
//     does the graded resolve with the frame's own stops and no table; an identity table FORCED
//     through the lookup moves no channel by more than one step;
//   * a known LUT — a channel rotation — maps every pixel to the ungraded pixel rotated;
//   * the committed warm and cool looks move the frame the way their names say;
//   * with grading off, the frame is the committed reference drawn by the frame's resolve before
//     this module existed.
//
// TOLERANCES ARE STATED WHERE THEY ARE USED, with the measured value beside each.

#include "device_fixture.h"
#include "frame_scene.h"
#include "golden.h"

#include <cy/rendering/grading/grading_renderer.h>
#include <cy/rendering/grading/look_file.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/rendering/post/exposure.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace cy;
using cy::grading_test::allocator;
using cy::grading_test::DeviceFixture;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
using cy::rendering::grading::ExposureReadback;
using cy::rendering::grading::GradingRenderer;
using cy::rendering::grading::GradingRendererDescription;
using cy::rendering::grading::ResolveCurve;

namespace {

/// The stops `FrameScene` writes into the frame's globals, which its own resolve applies.
constexpr f32 kSceneStops = -11.4F;

rendering::AutoExposureSettings frame_settings() noexcept {
    rendering::AutoExposureSettings settings;
    // The scene is lit by a 22 000 lux sun, so its surfaces meter around EV100 14; the range is
    // widened above the default's 16 so a four-times brighter scene is not clamped.
    settings.min_ev = 0.0F;
    settings.max_ev = 20.0F;
    settings.speed_brightening = 2.0F;
    settings.speed_darkening = 1.0F;
    return settings;
}

// --- Paths and pictures ----------------------------------------------------------------------

const char* test_path(const char* relative) noexcept {
    static char storage[4][1024];
    static u32 next = 0;
    char* out = storage[next++ % 4U];
    (void)std::snprintf(out, 1024, "%s/%s", CY_GRADING_TEST_DIR, relative);
    return out;
}

const char* content_path(const char* relative) noexcept {
    static char storage[4][1024];
    static u32 next = 0;
    char* out = storage[next++ % 4U];
    (void)std::snprintf(out, 1024, "%s/%s", CY_REPOSITORY_DIR, relative);
    return out;
}

void save(const char* name, const std::vector<u32>& texels) noexcept {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, Span<const u32>(texels.data(), texels.size()), kWidth, kHeight)
             .has_value()) {
        return;
    }
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    if (directory == nullptr || *directory == '\0') {
        directory = CY_TEST_BINARY_DIR;
    }
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (render_test::write_png(path, image).has_value()) {
        std::fprintf(stderr, "wrote %s (%ux%u)\n", path, kWidth, kHeight);
    }
}

[[nodiscard]] i32 channel(u32 texel, u32 which) noexcept {
    return static_cast<i32>((texel >> (which * 8U)) & 0xFFU);
}

/// The largest per-channel difference over the colour channels of two frames, and how many texels
/// differ at all.
struct FrameDifference {
    i32 largest = 0;
    usize texels = 0;
};

[[nodiscard]] FrameDifference difference(const std::vector<u32>& a,
                                         const std::vector<u32>& b) noexcept {
    FrameDifference out;
    for (usize index = 0; index < a.size() && index < b.size(); ++index) {
        i32 worst = 0;
        for (u32 which = 0; which < 3; ++which) {
            worst = std::max(worst, std::abs(channel(a[index], which) - channel(b[index], which)));
        }
        out.largest = std::max(out.largest, worst);
        out.texels += worst > 0 ? 1U : 0U;
    }
    return out;
}

// --- The frame, graded ------------------------------------------------------------------------

struct GradingCase {
    FrameScene* scene = nullptr;
    /// Null: the frame's own resolve, and nothing of this module in the frame.
    GradingRenderer* grading = nullptr;
    bool metered = false;
    bool graded = false;
    /// Every light and the ambient term multiplied by this: a scene brightness step.
    f32 light_scale = 1.0F;
    std::vector<rendering::GpuLight> lights;
};

void configure_case(rendering::assembly::AssemblyDescription& description, void* user) noexcept {
    const auto* frame = static_cast<const GradingCase*>(user);
    // PINNED, so two scenes rendering their first frames draw the same sub-pixel sample.
    description.pin_jitter = true;
    description.post.auto_exposure = frame->metered;
    description.post.colour_grading = frame->graded;
}

Status before_assemble(rendering::RenderGraph& graph, rendering::assembly::AssemblyView& /*view*/,
                       rendering::assembly::FrameSinks& sinks, void* user) noexcept {
    auto* frame = static_cast<GradingCase*>(user);
    if (frame->grading == nullptr) {
        return ok();
    }
    (void)frame->grading->import_state(graph);
    sinks.passes[static_cast<usize>(rendering::FramePassKind::PostProcess)] =
        frame->grading->post_process();
    return ok();
}

Status before_upload(rendering::pipeline::FrameUpload& upload, void* user) noexcept {
    auto* frame = static_cast<GradingCase*>(user);
    if (frame->light_scale == 1.0F) {
        return ok();
    }
    frame->lights.assign(upload.lights.begin(), upload.lights.end());
    for (rendering::GpuLight& light : frame->lights) {
        light.intensity *= frame->light_scale;
    }
    upload.lights = Span<const rendering::GpuLight>(frame->lights.data(), frame->lights.size());
    for (u32 which = 0; which < 3; ++which) {
        upload.view.ambient_and_occlusion[which] *= frame->light_scale;
    }
    return ok();
}

Status after_assemble(rendering::RenderGraph& graph, const rendering::FrameResources& resources,
                      void* user) noexcept {
    auto* frame = static_cast<GradingCase*>(user);
    if (frame->grading == nullptr) {
        return ok();
    }
    return frame->grading->declare_metering(graph, resources, kWidth, kHeight);
}

/// One scene with or without the graded resolve, and the frames it rendered.
class GradedScene {
public:
    GradedScene(DeviceFixture& fixture, bool attached, bool metered = false, bool graded = false,
                ResolveCurve curve = ResolveCurve::Reinhard)
        : scene_(allocator()) {
        frame_.scene = &scene_;
        frame_.metered = metered;
        frame_.graded = graded;
        FrameSceneHooks hooks;
        hooks.user = &frame_;
        hooks.configure = &configure_case;
        hooks.before_assemble = &before_assemble;
        hooks.before_upload = &before_upload;
        hooks.after_assemble = &after_assemble;
        scene_.set_hooks(hooks);
        const Status built = scene_.build(fixture.device());
        if (!built) {
            std::fprintf(stderr, "grading frame: build failed: %s\n", built.error().message);
            return;
        }
        scene_.set_read_back(true);
        if (attached) {
            GradingRendererDescription description;
            description.output_format = scene_.pipelines().setup().output_format;
            description.curve = curve;
            description.readback = true;
            const Status made = grading_.initialize(fixture.device(), allocator(), description);
            if (!made) {
                std::fprintf(stderr, "grading frame: initialize failed: %s\n",
                             made.error().message);
                return;
            }
            // The frame's own stops, so the graded resolve starts where the frame's resolve is.
            grading_.set_manual_stops(kSceneStops);
            frame_.grading = &grading_;
        }
        ready_ = true;
    }

    [[nodiscard]] bool render() {
        if (!ready_) {
            return false;
        }
        const Status rendered = scene_.render(RecordMode::Callbacks, report_);
        if (!rendered) {
            std::fprintf(stderr, "grading frame: render failed: %s\n", rendered.error().message);
            return false;
        }
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        return true;
    }

    /// Load a committed look and hand its table to the renderer.
    [[nodiscard]] bool apply_look(const char* path, bool& applied) {
        rendering::Look look;
        Array<Vec3> table(allocator());
        const Status loaded = rendering::grading::load_look(path, allocator(), look, table);
        if (!loaded) {
            std::fprintf(stderr, "grading frame: %s: %s\n", path, loaded.error().message);
            return false;
        }
        return grading_.set_lut(table.span(), look.lut_size, applied).has_value();
    }

    void set_light_scale(f32 scale) noexcept { frame_.light_scale = scale; }
    [[nodiscard]] const std::vector<u32>& pixels() const { return pixels_; }
    [[nodiscard]] GradingRenderer& grading() { return grading_; }
    [[nodiscard]] const rendering::assembly::AssemblyReport& report() const { return report_; }

private:
    FrameScene scene_;
    GradingRenderer grading_;
    GradingCase frame_;
    rendering::assembly::AssemblyReport report_{};
    std::vector<u32> pixels_;
    bool ready_ = false;
};

}  // namespace

CY_TEST_CASE("auto-exposure converges within its adaptation time and follows a brightness step") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    GradedScene scene(fixture, true, true);
    const rendering::AutoExposureSettings settings = frame_settings();
    constexpr f32 kDelta = 1.0F / 30.0F;
    scene.grading().set_delta_seconds(kDelta);

    // Learn where the scene meters, by metering it once without adapting.
    scene.grading().set_automatic(settings, 10.0F);
    scene.grading().set_delta_seconds(0.0F);
    CY_REQUIRE(scene.render());
    ExposureReadback state;
    CY_REQUIRE(scene.grading().read_exposure(state).has_value());
    const f32 learned = state.target_ev100;
    std::fprintf(stderr, "the scene meters at EV100 %.3f\n", static_cast<double>(learned));
    CY_REQUIRE((learned > settings.min_ev + 4.0F && learned < settings.max_ev - 3.0F));

    // Every frame follows the host's `adapt_ev100` from the previous frame's EV toward the target
    // the device metered: the direction's speed, the exponential, the clamp.
    f32 worst_follow = 0.0F;
    f32 previous = 0.0F;
    const auto run = [&](u32 frames, f32 scale) {
        scene.set_light_scale(scale);
        std::vector<f32> gaps;
        for (u32 frame = 0; frame < frames; ++frame) {
            CY_CHECK(scene.render());
            CY_CHECK(scene.grading().read_exposure(state).has_value());
            const f32 host = rendering::adapt_ev100(previous, state.target_ev100, kDelta, settings);
            worst_follow = std::max(worst_follow, std::fabs(host - state.current_ev100));
            previous = state.current_ev100;
            gaps.push_back(state.target_ev100 - state.current_ev100);
        }
        return gaps;
    };

    // CONVERGENCE: from four stops under, within 5 % of the gap by the stated time and not before
    // the stated 50 % time — the adaptation is the configured speed, not merely "eventually".
    const f32 start = learned - 4.0F;
    scene.grading().set_automatic(settings, start);
    scene.grading().set_delta_seconds(kDelta);
    previous = start;
    const f32 deadline = rendering::adaptation_seconds(settings.speed_brightening, 0.05F);
    const u32 frames = static_cast<u32>(std::ceil(deadline / kDelta));
    const std::vector<f32> rising = run(frames + 5U, 1.0F);
    const f32 half_way = rendering::adaptation_seconds(settings.speed_brightening, 0.5F);
    const u32 before_half = static_cast<u32>(std::floor(half_way / kDelta)) - 2U;
    std::fprintf(stderr,
                 "converge: gap %.4f after %u frames (%.3f s), %.4f at frame %u; worst follow "
                 "%.2e\n",
                 static_cast<double>(rising[frames - 1U]), frames, static_cast<double>(deadline),
                 static_cast<double>(rising[before_half]), before_half,
                 static_cast<double>(worst_follow));
    CY_CHECK_LE(std::fabs(rising[frames - 1U]), (0.05F * 4.0F) + 0.02F);
    CY_CHECK_GT(rising[before_half], 0.5F * 4.0F);

    // THE RESOLVE APPLIES WHAT THE PREVIOUS FRAME ADAPTED TO, byte for byte against the same EV
    // pushed by hand in a second scene that has rendered as many frames — the temporal history is
    // scene-referred, so it is the same history whatever either scene exposed by, and the metered
    // path and the manual path are then one multiply of one colour.
    const f32 applied = state.current_ev100;
    CY_REQUIRE(scene.render());
    const std::vector<u32> metered = scene.pixels();
    CY_REQUIRE(scene.grading().read_exposure(state).has_value());
    previous = state.current_ev100;
    const u32 rendered = frames + 7U;
    GradedScene manual(fixture, true);
    manual.grading().set_manual_ev100(applied);
    for (u32 frame = 0; frame < rendered; ++frame) {
        CY_REQUIRE(manual.render());
    }
    const FrameDifference pushed = difference(metered, manual.pixels());
    std::fprintf(stderr, "metered frame against the same EV pushed: %zu texels differ\n",
                 pushed.texels);
    CY_CHECK_EQ(pushed.texels, usize{0});
    CY_CHECK_GT(scene.grading().report().metered_resolves, 0U);

    // THE STEP: four times the light is two stops, reached at the brightening speed; back again at
    // the darkening speed. The temporal history the frame meters blends the step in over a few
    // frames, so the deadline is doubled and the per-frame agreement carries the speed.
    (void)run(10, 1.0F);
    const f32 before_step = state.target_ev100;
    const u32 step_frames = 2U * frames;
    const std::vector<f32> brighter = run(step_frames, 4.0F);
    const f32 after_step = state.target_ev100;
    const std::vector<f32> darker = run(step_frames, 1.0F);
    const f32 back = state.target_ev100;
    std::fprintf(stderr,
                 "step: target %.3f -> %.3f -> %.3f; gap after brightening %.4f, after darkening "
                 "%.4f; worst follow %.2e\n",
                 static_cast<double>(before_step), static_cast<double>(after_step),
                 static_cast<double>(back), static_cast<double>(brighter.back()),
                 static_cast<double>(darker.back()), static_cast<double>(worst_follow));
    // MEASURED: see the line above — 1.85 stops. Four times the light cannot meter as more than two
    // stops, and it meters as less only by what in the percentile window the lights do not light:
    // the cleared background and the unlit faces the ambient term alone reaches. Then back to where
    // it was.
    CY_CHECK_GT(after_step - before_step, 1.75F);
    CY_CHECK_LE(after_step - before_step, 2.02F);
    // Back to within a tenth: the metered target of this still scene wanders by a few hundredths
    // from frame to frame as its temporal history settles (MEASURED 13.935 to 13.978 between runs,
    // 0.055 before and after the step in one), which is the history's and not the adaptation's.
    CY_CHECK_LE(std::fabs(back - before_step), 0.1F);
    CY_CHECK_LE(std::fabs(brighter.back()), (0.05F * 2.0F) + 0.02F);
    // Darkening is half as fast: the same time leaves more of the gap.
    CY_CHECK_GT(std::fabs(darker.back()), std::fabs(brighter.back()));
    CY_CHECK_LE(worst_follow, 1e-3F);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("a manual EV100 one stop higher halves every unclipped pixel") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // A LINEAR CURVE, so the output IS the exposed radiance and the factor is readable per pixel:
    // the `Rgba8Unorm` output holds round(255 x).
    // Two scenes, one frame each, so the temporal history both resolve is the same first frame.
    const f32 ev = rendering::ev100_for_exposure_stops(kSceneStops) + 1.5F;
    GradedScene lighter(fixture, true, false, false, ResolveCurve::None);
    lighter.grading().set_manual_ev100(ev);
    CY_REQUIRE(lighter.render());
    const std::vector<u32> bright = lighter.pixels();
    GradedScene darker(fixture, true, false, false, ResolveCurve::None);
    darker.grading().set_manual_ev100(ev + 1.0F);
    CY_REQUIRE(darker.render());
    const std::vector<u32> dark = darker.pixels();

    // Every channel whose bright value is well inside the range: the dark one is half of it, give
    // or take the two roundings — |round(x/2) - round(x)/2| <= 0.75, so one step.
    u32 measured = 0;
    u32 outside = 0;
    f64 ratio = 0.0;
    for (usize index = 0; index < bright.size(); ++index) {
        for (u32 which = 0; which < 3; ++which) {
            const i32 high = channel(bright[index], which);
            const i32 low = channel(dark[index], which);
            if (high < 64 || high > 250) {
                continue;
            }
            ++measured;
            outside +=
                std::fabs(static_cast<f32>(low) - (static_cast<f32>(high) * 0.5F)) > 1.0F ? 1U : 0U;
            ratio += static_cast<f64>(low) / static_cast<f64>(high);
        }
    }
    const f64 mean = measured > 0 ? ratio / measured : 0.0;
    std::fprintf(stderr,
                 "EV %.3f -> %.3f: %u channels measured, %u outside one step, mean ratio %.5f\n",
                 static_cast<double>(ev), static_cast<double>(ev + 1.0F), measured, outside, mean);
    CY_REQUIRE(measured > 10000U);
    CY_CHECK_EQ(outside, 0U);
    CY_CHECK_LE(std::fabs(mean - 0.5), 0.004);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "an identity LUT and neutral parameters draw the frame without grading, byte for "
    "byte") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // EVERY FRAME COMPARED IS A SCENE'S FIRST: the pipeline scene resolves a temporal history,
    // and two frames of one scene differ at edges however they are graded.
    GradedScene plain(fixture, false);
    CY_REQUIRE(plain.render());
    std::vector<Vec3> neutral(static_cast<usize>(rendering::kDisplayLutSize) *
                              rendering::kDisplayLutSize * rendering::kDisplayLutSize);
    CY_REQUIRE(rendering::bake_display_lut(rendering::DisplayGrade{}, rendering::kDisplayLutSize,
                                           neutral.data(), neutral.size()));
    const Span<const Vec3> table(neutral.data(), neutral.size());

    // THE GRADED RESOLVE WITH NOTHING TO DO is the frame's own: its stops, its curve, no table.
    GradedScene nothing(fixture, true, false, true);
    CY_REQUIRE(nothing.render());
    const FrameDifference resolve = difference(plain.pixels(), nothing.pixels());

    // NEUTRAL CONTROLS AND AN IDENTITY CUBE are recognised and the lookup is not applied.
    GradedScene controls_scene(fixture, true, false, true);
    bool neutral_applied = true;
    CY_REQUIRE(controls_scene.grading()
                   .set_lut(table, rendering::kDisplayLutSize, neutral_applied)
                   .has_value());
    CY_REQUIRE(controls_scene.render());
    const FrameDifference controls = difference(plain.pixels(), controls_scene.pixels());
    GradedScene cube_scene(fixture, true, false, true);
    bool identity_applied = true;
    CY_REQUIRE(cube_scene.apply_look(test_path("luts/identity.cygrade"), identity_applied));
    CY_REQUIRE(cube_scene.render());
    const FrameDifference cube = difference(plain.pixels(), cube_scene.pixels());

    // AND THE LOOKUP ITSELF, FORCED: the identity table through the device's trilinear fetch, at
    // half precision. Not byte-identical by construction, and bounded instead.
    GradedScene forced_scene(fixture, true, false, true);
    CY_REQUIRE(forced_scene.grading().force_lut(table, rendering::kDisplayLutSize).has_value());
    CY_REQUIRE(forced_scene.render());
    const FrameDifference forced = difference(plain.pixels(), forced_scene.pixels());
    std::fprintf(stderr,
                 "against the frame without grading: graded resolve %zu texels (worst %d), neutral "
                 "controls %zu (applied %d), identity cube %zu (applied %d), identity FORCED "
                 "through the lookup %zu texels, worst %d step(s)\n",
                 resolve.texels, resolve.largest, controls.texels, neutral_applied ? 1 : 0,
                 cube.texels, identity_applied ? 1 : 0, forced.texels, forced.largest);
    CY_CHECK_EQ(resolve.texels, usize{0});
    CY_CHECK_FALSE(neutral_applied);
    CY_CHECK_EQ(controls.texels, usize{0});
    CY_CHECK_FALSE(identity_applied);
    CY_CHECK_EQ(cube.texels, usize{0});
    // MEASURED: see the line above. Half precision stores the encoding to 2^-12 near white, which
    // is 0.2 % of a display value — half a step at 255 — so one step is the physical bound.
    CY_CHECK_LE(forced.largest, 1);
    CY_CHECK_EQ(forced_scene.grading().report().graded_resolves, 1U);
    CY_CHECK_EQ(controls_scene.grading().report().graded_resolves, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("a known LUT maps every pixel to its expected value on the device") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    GradedScene plain(fixture, false);
    CY_REQUIRE(plain.render());
    GradedScene rotated(fixture, true, false, true);
    bool applied = false;
    CY_REQUIRE(rotated.apply_look(test_path("luts/rotate.cygrade"), applied));
    CY_REQUIRE(applied);
    CY_REQUIRE(rotated.render());
    save("grading-rotated.png", rotated.pixels());

    // The expected pixel is the ungraded one with red taking green, green blue and blue red.
    u32 outside = 0;
    i32 worst = 0;
    u32 moved = 0;
    for (usize index = 0; index < plain.pixels().size(); ++index) {
        const u32 before = plain.pixels()[index];
        const u32 after = rotated.pixels()[index];
        const i32 expected[3] = {channel(before, 1), channel(before, 2), channel(before, 0)};
        i32 largest = 0;
        for (u32 which = 0; which < 3; ++which) {
            largest = std::max(largest, std::abs(channel(after, which) - expected[which]));
        }
        worst = std::max(worst, largest);
        outside += largest > 1 ? 1U : 0U;
        moved += channel(before, 0) != channel(before, 1) ? 1U : 0U;
    }
    std::fprintf(stderr,
                 "rotation: %u of %zu texels more than one step from their expected value, worst "
                 "%d; %u texels whose red and green differ\n",
                 outside, plain.pixels().size(), worst, moved);
    // THE CONTROL: the scene is coloured, so a rotation that did nothing would fail.
    CY_REQUIRE(moved > 10000U);
    // MEASURED: see the line above. A permutation is linear in the encoding, so the only error is
    // the table's half precision — the same one-step bound as the forced identity.
    CY_CHECK_EQ(outside, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the warm look warms the frame and the cool look cools it") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    GradedScene plain(fixture, false);
    CY_REQUIRE(plain.render());
    const auto balance = [](const std::vector<u32>& texels) {
        f64 red = 0.0;
        f64 blue = 0.0;
        for (const u32 texel : texels) {
            red += channel(texel, 0);
            blue += channel(texel, 2);
        }
        return red / std::max(blue, 1.0);
    };
    const f64 neutral = balance(plain.pixels());
    f64 looks[2] = {};
    const char* paths[2] = {"content/beauty/looks/warm.cygrade",
                            "content/beauty/looks/cool.cygrade"};
    const char* names[2] = {"grading-warm.png", "grading-cool.png"};
    for (u32 which = 0; which < 2; ++which) {
        GradedScene graded(fixture, true, false, true);
        bool applied = false;
        CY_REQUIRE(graded.apply_look(content_path(paths[which]), applied));
        CY_REQUIRE(applied);
        CY_REQUIRE(graded.render());
        save(names[which], graded.pixels());
        looks[which] = balance(graded.pixels());
    }
    save("grading-none.png", plain.pixels());
    std::fprintf(stderr, "red / blue over the frame: none %.4f, warm %.4f, cool %.4f\n", neutral,
                 looks[0], looks[1]);
    // MEASURED: see the line above. Each look moves the balance by several per cent.
    CY_CHECK_GT(looks[0], neutral * 1.03);
    CY_CHECK_LT(looks[1], neutral * 0.97);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("with grading off the frame is the committed frame from before grading existed") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    GradedScene plain(fixture, false);
    CY_REQUIRE(plain.render());
    // `references/grading_absent.png`: this scene rendered by the frame's own resolve from the
    // SPIR-V at main before this module existed (1fe6446), with `CY_RENDER_UPDATE_GOLDEN=1`.
    // Nothing on that path changed here, so a change to it is somebody else's and still goes red.
    render_test::Image rendered(allocator());
    CY_REQUIRE(render_test::adopt(rendered,
                                  Span<const u32>(plain.pixels().data(), plain.pixels().size()),
                                  kWidth, kHeight)
                   .has_value());
    const char* path = test_path("references/grading_absent.png");
    const char* update = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    if (update != nullptr && update[0] != '\0' && update[0] != '0') {
        CY_CHECK(render_test::write_png(path, rendered).has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     path);
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        return;
    }
    render_test::Image reference(allocator());
    const Status read = render_test::read_png(path, reference);
    if (!read) {
        std::fprintf(stderr, "%s: %s\n", path, read.error().message);
    }
    CY_REQUIRE(read.has_value());
    const render_test::Comparison comparison = render_test::compare(reference, rendered);
    std::fprintf(stderr,
                 "against the frame before grading: %u differing (%u off edge), edge budget %u, "
                 "worst delta %u at (%u, %u)\n",
                 comparison.differing, comparison.differing_off_edge, comparison.edge_texels,
                 comparison.max_channel_delta, comparison.worst_x, comparison.worst_y);
    CY_CHECK(comparison.comparable);
    CY_CHECK_EQ(comparison.differing_off_edge, 0U);
    CY_CHECK_LE(comparison.differing, comparison.edge_texels);
    CY_CHECK_EQ(comparison.differing, 0U);
    CY_CHECK_EQ(comparison.max_channel_delta, 0U);
    // The chain the frame ran names no grading stage.
    for (u32 index = 0; index < plain.report().post_stages; ++index) {
        CY_CHECK(plain.report().post_stage[index] != rendering::PostStage::ColourGrading);
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
