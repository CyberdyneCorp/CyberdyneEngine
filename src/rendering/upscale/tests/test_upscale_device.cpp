// SPDX-License-Identifier: MIT
// Temporal upscaling on the device. `render.upscale`, Vulkan only, with validation and
// synchronisation validation on.
//
// The pipeline suites' scene (`frame_scene.h`), its output 480x270, rendered at each mode's render
// extent and reconstructed to the output by `TemporalUpscalePass` at the frame's temporal stage:
//
//   (a) a still frame converges to the native frame in every mode, within a stated error;
//   (b) the device reconstruction is the host reference's, texel for texel, over inputs read back
//       from a frame with the camera moving, particles drawn and the reactive mask on;
//   (c) a box sliding across the frame leaves no trail behind it;
//   (d) a box that jumps away reveals the floor rebuilt from this frame, not the box's history;
//   (e) the interface is drawn at the OUTPUT extent: its pixels are the same bytes in every mode;
//   (f) the budget arbiter moves the render extent, and the history survives the move;
//   (g) motion blur after the upscaler reads the depth and motion it resampled to the output.
//
// Each image case has a CONTROL: the same run with the mechanism the case is about taken away.
// The frame without the upscaler is `render.upscale_off`'s, a target of its own.

#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/arbiter/arbiter.h>
#include <cy/rendering/motion_blur/motion_blur_pass.h>
#include <cy/rendering/upscale/upscale_pass.h>
#include <cy/test/test.h>
#include <cy/ui/layout.h>
#include <cy/ui/render/ui_renderer.h>
#include <cy/ui/text/builtin_font.h>
#include <cy/ui/text/text_painter.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace cy;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
using namespace cy::rendering;
using namespace cy::rendering::upscale;

namespace {

constexpr usize kPixels = static_cast<usize>(kWidth) * kHeight;
constexpr Extent2D kOutput{kWidth, kHeight};
/// The scene's exposure: `frame_scene.cpp` uploads -11.4 stops.
const f32 kExposure = std::exp2(-11.4F);
/// Frames a still case accumulates: longer than every mode's jitter cycle.
constexpr u32 kConverge = 40;
constexpr f32 kFloorTop = -1.9F + 0.125F;
constexpr f32 kBoxHalf = 0.5F;
constexpr Vec3 kBoxStart{-1.6F, kFloorTop + kBoxHalf, -5.0F};

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
        description.application_name = "cy_test_render_upscale";
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

[[nodiscard]] u32 channel(u32 texel, u32 index) noexcept {
    return (texel >> (index * 8U)) & 0xFFU;
}

/// Mean absolute difference per colour channel, in 8-bit steps, over the pixels `inside` admits.
template <typename Inside>
[[nodiscard]] f64 mean_error(const std::vector<u32>& a, const std::vector<u32>& b,
                             Inside inside) noexcept {
    f64 sum = 0.0;
    usize count = 0;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            if (!inside(x, y)) {
                continue;
            }
            const usize at = (static_cast<usize>(y) * kWidth) + x;
            for (u32 c = 0; c < 3U; ++c) {
                const auto da = static_cast<i32>(channel(a[at], c));
                const auto db = static_cast<i32>(channel(b[at], c));
                sum += static_cast<f64>(da > db ? da - db : db - da);
            }
            ++count;
        }
    }
    return count == 0 ? 0.0 : sum / (3.0 * static_cast<f64>(count));
}

[[nodiscard]] f64 mean_error(const std::vector<u32>& a, const std::vector<u32>& b) noexcept {
    return mean_error(a, b, [](u32, u32) { return true; });
}

/// Peak signal to noise, in decibels, over 8-bit colour.
[[nodiscard]] f64 psnr(const std::vector<u32>& a, const std::vector<u32>& b) noexcept {
    f64 sum = 0.0;
    for (usize at = 0; at < a.size() && at < b.size(); ++at) {
        for (u32 c = 0; c < 3U; ++c) {
            const f64 d = static_cast<f64>(channel(a[at], c)) - static_cast<f64>(channel(b[at], c));
            sum += d * d;
        }
    }
    const f64 mse = sum / (3.0 * static_cast<f64>(a.size()));
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

/// The field: box 1 in front, where the moving cases slide it; the rest a still row behind.
void place_box(u32 which, Vec3& centre, f32& half, void* /*user*/) noexcept {
    if (which == 1U) {
        centre = kBoxStart;
        half = kBoxHalf;
        return;
    }
    half = 0.3F + (0.06F * static_cast<f32>(which % 3U));
    centre = Vec3{-4.5F + static_cast<f32>(which - 2U), kFloorTop + half, -9.0F};
}

// --- An interface over the frame -------------------------------------------------------------

constexpr u16 kGlyphPage = 1;
constexpr u32 kPanelColour = 0xFF182028U;
constexpr u32 kRedColour = 0xFFE0302AU;
constexpr u32 kTextColour = 0xFFFFD040U;

/// Two panels and a label in the built-in font: what the interface case draws over every mode.
struct Interface {
    ui::ElementStore store{allocator()};
    text::TextServer server;
    ui::TextPainter text{allocator()};
    ui::render::UiRenderer renderer{allocator()};
    ui::PrimitiveBuffer buffer{allocator()};

    [[nodiscard]] bool start(rhi::Device& device, rhi::Format output) noexcept {
        ui::render::UiRendererDescription description;
        description.width = kWidth;
        description.height = kHeight;
        description.output_format = output;
        if (!renderer.create(device, description).has_value() ||
            !server.start(text::TextServerConfig{}).has_value() ||
            !text.start(server, ui::builtin_font(), kGlyphPage).has_value()) {
            return false;
        }
        const u32 extent = text.atlas_extent();
        if (!renderer
                 .upload_atlas(kGlyphPage, rhi::Format::R8Unorm, extent, extent,
                               text.atlas_pixels())
                 .has_value()) {
            return false;
        }
        auto root = store.create(ui::kNoElement, Name::intern("screen"));
        if (!root.has_value()) {
            return false;
        }
        store.layout_input(*root)->model = ui::LayoutModel::Absolute;
        const ui::ElementId panel = box(*root, 12.0F, 12.0F, 150.0F, 40.0F, kPanelColour);
        (void)box(*root, 300.0F, 200.0F, 61.0F, 37.0F, kRedColour);
        ui::TextStyle style;
        style.colour = kTextColour;
        style.pixel_scale = 2;
        if (!text.set_text(panel, "UPSCALE 1440", style).has_value()) {
            return false;
        }
        ui::ScaleSettings settings;
        settings.mode = ui::ScaleMode::FixedPixel;
        ui::LayoutReport laid{};
        const Vec2 viewport{static_cast<f32>(kWidth), static_cast<f32>(kHeight)};
        if (!ui::layout(store, settings, viewport, &text, laid).has_value()) {
            return false;
        }
        ui::FlattenReport flattened{};
        return ui::flatten(store, ui::Rect{0.0F, 0.0F, viewport.x, viewport.y}, buffer, flattened,
                           &text)
            .has_value();
    }

    [[nodiscard]] ui::ElementId box(ui::ElementId parent, f32 x, f32 y, f32 w, f32 h,
                                    u32 colour) noexcept {
        auto made = store.create(parent, Name::intern("box"));
        if (!made.has_value()) {
            return ui::kNoElement;
        }
        ui::LayoutInput* input = store.layout_input(*made);
        input->model = ui::LayoutModel::Absolute;
        input->offset_min = Vec2{x, y};
        input->offset_max = Vec2{x + w, y + h};
        store.paint(*made)->background = colour;
        return *made;
    }

    ~Interface() { renderer.destroy(); }
};

// --- One run: the scene, the upscaler, and what they produce ----------------------------------

struct RunOptions {
    /// False: the scene's own temporal antialiasing at the output extent — the frame from before.
    bool upscale = true;
    UpscaleMode mode = UpscaleMode::Quality;
    UpscaleSettings settings{};
    bool readback = false;
    /// Particles in the transparent stage, and the reactive mask derived from them.
    bool particles = false;
    bool reactive = false;
    bool motion_blur = false;
    bool interface = false;
};

[[nodiscard]] UpscaleSettings settings_for(UpscaleMode mode) noexcept {
    UpscaleSettings settings;
    settings.mode = mode;
    settings.exposure = kExposure;
    settings.sharpness = 0.0F;
    return settings;
}

class Run {
public:
    Run(DeviceFixture& fixture, const RunOptions& options)
        : fixture_(fixture), options_(options), scene_(allocator()) {
        render_ = options.upscale ? render_extent_for(kOutput, upscale_mode_scale(options.mode))
                                  : kOutput;
        FrameSceneHooks hooks;
        hooks.user = this;
        hooks.configure = &configure;
        hooks.before_assemble = &before_assemble;
        hooks.place_box = &place_box;
        scene_.set_hooks(hooks);
        if (options_.upscale) {
            UpscalePassDescription description;
            description.output = kOutput;
            description.readback = options_.readback;
            if (const Status made = pass_.create(allocator(), fixture.device(), description);
                !made) {
                std::fprintf(stderr, "upscale run: pass failed: %s\n", made.error().message);
                return;
            }
            pass_.set_settings(options_.settings);
        }
        if (const Status built = scene_.build(fixture.device()); !built) {
            std::fprintf(stderr, "upscale run: build failed: %s\n", built.error().message);
            return;
        }
        scene_.set_read_back(true);
        if (options_.upscale && !pass_.bind_temporal(scene_.assembly().temporal()).has_value()) {
            return;
        }
        if (options_.motion_blur) {
            motion_blur::MotionBlurPassDescription description;
            description.width = kWidth;
            description.height = kHeight;
            if (!blur_.create(allocator(), fixture.device(), description).has_value()) {
                return;
            }
        }
        if (options_.interface) {
            interface_ = std::make_unique<Interface>();
            if (!interface_->start(fixture.device(), scene_.pipelines().setup().output_format)) {
                return;
            }
        }
        ready_ = true;
    }
    ~Run() {
        (void)fixture_.device().wait_idle();
        interface_.reset();
        blur_.destroy();
        pass_.destroy();
        scene_.release();
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_; }

    /// One frame with box 1 at `centre`.
    [[nodiscard]] bool frame(Vec3 centre = kBoxStart) {
        if (!ready_ || !scene_.move_box(1, centre).has_value()) {
            return false;
        }
        if (interface_ != nullptr && !interface_->renderer.submit(interface_->buffer, 1.0F)) {
            return false;
        }
        const Status rendered = scene_.render(
            options_.particles ? RecordMode::CallbacksAndParticles : RecordMode::Callbacks,
            report_);
        if (!rendered) {
            std::fprintf(stderr, "upscale run: render failed: %s\n", rendered.error().message);
            return false;
        }
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        return true;
    }

    [[nodiscard]] bool frames(u32 count, Vec3 centre = kBoxStart) {
        for (u32 index = 0; index < count; ++index) {
            if (!frame(centre)) {
                return false;
            }
        }
        return true;
    }

    /// Render at another extent from the next frame: dynamic resolution.
    [[nodiscard]] bool set_render(Extent2D extent) noexcept {
        render_ = extent;
        return scene_.assembly().set_render_extent(extent.width, extent.height).has_value();
    }

    [[nodiscard]] const std::vector<u32>& pixels() const noexcept { return pixels_; }
    [[nodiscard]] TemporalUpscalePass& pass() noexcept { return pass_; }
    [[nodiscard]] FrameScene& scene() noexcept { return scene_; }
    [[nodiscard]] const assembly::AssemblyReport& report() const noexcept { return report_; }
    [[nodiscard]] Extent2D render() const noexcept { return render_; }

private:
    static void configure(assembly::AssemblyDescription& description, void* user) noexcept {
        const auto* run = static_cast<const Run*>(user);
        description.pin_jitter = true;
        description.pinned_jitter_index = 0;
        description.post.motion_blur = run->options_.motion_blur;
        if (!run->options_.upscale) {
            return;
        }
        description.post.temporal_upscaling = true;
        description.produced_temporal_stage = true;
        description.reactive_mask = run->options_.reactive;
        description.render_width = run->render_.width;
        description.render_height = run->render_.height;
        description.temporal.jitter = jitter_config_for(kOutput, run->render_);
    }

    static Status before_assemble(RenderGraph& graph, assembly::AssemblyView& view,
                                  assembly::FrameSinks& sinks, void* user) noexcept {
        auto* run = static_cast<Run*>(user);
        if (run->options_.upscale) {
            sinks.temporal = stage_of(run->pass_);
            if (run->options_.reactive) {
                sinks.opaque_copy = run->pass_.opaque_copy_stage();
            }
        }
        if (run->options_.motion_blur) {
            // A still camera: last frame's camera-relative projection is this frame's.
            motion_blur::MotionBlurView blur;
            blur.width = kWidth;
            blur.height = kHeight;
            blur.projection = run->scene_.projection();
            blur.relative_to_clip = run->scene_.projection() * run->scene_.relative_view();
            blur.previous_relative_to_clip = blur.relative_to_clip;
            if (Status set = run->blur_.set_view(blur); !set) {
                return set;
            }
            view.motion_blur = run->blur_.import_target(graph);
            sinks.motion_blur = run->blur_.stage();
        }
        if (run->interface_ != nullptr) {
            sinks.ui = run->interface_->renderer.stage();
        }
        return ok();
    }

    DeviceFixture& fixture_;
    RunOptions options_;
    FrameScene scene_;
    TemporalUpscalePass pass_;
    motion_blur::MotionBlurPass blur_;
    std::unique_ptr<Interface> interface_;
    Extent2D render_{};
    assembly::AssemblyReport report_{};
    std::vector<u32> pixels_;
    bool ready_ = false;
};

/// The picture a mode converges to on a still frame with box 1 at `centre`: the native one — the
/// upscaler at the output extent — unless another mode is asked for.
[[nodiscard]] bool native_reference(DeviceFixture& fixture, std::vector<u32>& out,
                                    Vec3 centre = kBoxStart,
                                    UpscaleMode mode = UpscaleMode::NativeAntialiasing) {
    RunOptions options;
    options.mode = mode;
    options.settings = settings_for(mode);
    Run run(fixture, options);
    if (!run.frames(kConverge, centre)) {
        return false;
    }
    out = run.pixels();
    return true;
}

}  // namespace

// --- (a) ---------------------------------------------------------------------------------------

namespace {

/// The mode's frame after `kConverge` frames against the native one: mean absolute error per
/// channel in 8-bit steps, and the first frame's for comparison.
void check_convergence(UpscaleMode mode, f64 bound) {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> native;
    CY_REQUIRE(native_reference(fixture, native));
    RunOptions options;
    options.mode = mode;
    options.settings = settings_for(mode);
    Run run(fixture, options);
    CY_REQUIRE(run.ready());
    CY_REQUIRE(run.frame());
    const f64 first = mean_error(run.pixels(), native);
    CY_REQUIRE(run.frames(kConverge - 1U));
    const f64 converged = mean_error(run.pixels(), native);
    std::fprintf(stderr,
                 "(a) %-12s render %ux%u: first frame %.3f, converged %.3f (8-bit), %.1f dB\n",
                 upscale_mode_name(mode), run.render().width, run.render().height, first, converged,
                 psnr(run.pixels(), native));
    char name[64];
    (void)std::snprintf(name, sizeof(name), "upscale-%s.png", upscale_mode_name(mode));
    save(name, run.pixels());
    if (mode == UpscaleMode::Performance) {
        save("upscale-native.png", native);
    }
    CY_CHECK_LT(converged, bound);
    CY_CHECK_LT(converged * 2.0, first);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

}  // namespace

CY_TEST_CASE("(a) a still frame converges to the native frame (quality)") {
    check_convergence(UpscaleMode::Quality, 1.5);
}

CY_TEST_CASE("(a) a still frame converges to the native frame (balanced)") {
    check_convergence(UpscaleMode::Balanced, 1.8);
}

CY_TEST_CASE("(a) a still frame converges to the native frame (performance)") {
    check_convergence(UpscaleMode::Performance, 2.0);
}

// --- (b) ---------------------------------------------------------------------------------------

CY_TEST_CASE("(b) the device reconstruction is the host reference's") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    RunOptions options;
    options.mode = UpscaleMode::Quality;
    options.settings = settings_for(UpscaleMode::Quality);
    options.settings.sharpness = 0.5F;
    options.readback = true;
    options.particles = true;
    options.reactive = true;
    Run run(fixture, options);
    CY_REQUIRE(run.ready());
    // The camera walks and the box slides, so every term — motion, reprojection, rejection, the
    // clip, the locks and the reactive mask — has something to do.
    for (u32 frame = 0; frame < 8U; ++frame) {
        run.scene().set_eye(Vec3{0.05F * static_cast<f32>(frame), 0.0F, 0.0F});
        CY_REQUIRE(run.frame(
            Vec3{kBoxStart.x + (0.2F * static_cast<f32>(frame)), kBoxStart.y, kBoxStart.z}));
    }
    const Extent2D render = run.render();
    const usize render_pixels = static_cast<usize>(render.width) * render.height;
    std::vector<Vec4> color(render_pixels);
    std::vector<f32> depth(render_pixels);
    std::vector<Vec2> velocity(render_pixels);
    std::vector<Vec4> opaque(render_pixels);
    std::vector<Vec4> previous_history(kPixels);
    std::vector<Vec4> history(kPixels);
    std::vector<Vec2> previous_state(kPixels);
    std::vector<Vec2> state(kPixels);
    std::vector<Vec4> output(kPixels);
    UpscaleReadback readback;
    readback.color = Span<Vec4>(color.data(), color.size());
    readback.depth = Span<f32>(depth.data(), depth.size());
    readback.velocity = Span<Vec2>(velocity.data(), velocity.size());
    readback.opaque = Span<Vec4>(opaque.data(), opaque.size());
    readback.previous_history = Span<Vec4>(previous_history.data(), previous_history.size());
    readback.history = Span<Vec4>(history.data(), history.size());
    readback.previous_state = Span<Vec2>(previous_state.data(), previous_state.size());
    readback.state = Span<Vec2>(state.data(), state.size());
    readback.output = Span<Vec4>(output.data(), output.size());
    CY_REQUIRE(run.pass().read_back(readback).has_value());

    const UpscaleConstants& constants = run.pass().constants();
    CY_REQUIRE((constants.flags() & kUpscaleHistoryValid) != 0U);
    CY_REQUIRE((constants.flags() & kUpscaleReactive) != 0U);
    UpscaleInputs inputs;
    inputs.color = Span<const Vec4>(color.data(), color.size());
    inputs.depth = Span<const f32>(depth.data(), depth.size());
    inputs.velocity = Span<const Vec2>(velocity.data(), velocity.size());
    inputs.opaque = Span<const Vec4>(opaque.data(), opaque.size());
    inputs.history = Span<const Vec4>(previous_history.data(), previous_history.size());
    inputs.previous_state = Span<const Vec2>(previous_state.data(), previous_state.size());
    std::vector<Vec4> expected(kPixels);
    std::vector<Vec2> expected_state(kPixels);
    CY_REQUIRE(upscale_reference(inputs, constants, Span<Vec4>(expected.data(), expected.size()),
                                 Span<Vec2>(expected_state.data(), expected_state.size()))
                   .has_value());

    // The device stores half floats: a texel agrees when every channel is within a half-float step
    // of the host's, relative, plus a floor for the darkest values. A handful may straddle a
    // branch — a texel whose nearest sample, ridge or rejection the device's float arithmetic
    // decided the other way — and are counted rather than allowed to hide.
    usize disagreeing = 0;
    usize locks_disagreeing = 0;
    f32 worst = 0.0F;
    for (usize at = 0; at < kPixels; ++at) {
        const f32 deltas[4] = {
            std::fabs(history[at].x - expected[at].x), std::fabs(history[at].y - expected[at].y),
            std::fabs(history[at].z - expected[at].z), std::fabs(history[at].w - expected[at].w)};
        const f32 scales[4] = {std::fabs(expected[at].x), std::fabs(expected[at].y),
                               std::fabs(expected[at].z), std::fabs(expected[at].w)};
        bool agrees = true;
        for (u32 c = 0; c < 4U; ++c) {
            const f32 relative = deltas[c] / std::fmax(scales[c], 1.0F);
            worst = std::fmax(worst, relative);
            agrees = agrees && deltas[c] <= (scales[c] * 4.0e-3F) + 0.5F;
        }
        disagreeing += agrees ? 0U : 1U;
        locks_disagreeing += (std::fabs(state[at].x - expected_state[at].x) > 1.0e-3F ||
                              state[at].y != expected_state[at].y)
                                 ? 1U
                                 : 0U;
    }
    std::fprintf(stderr, "(b) %zu of %zu texels disagree, %zu states; worst relative %.5f\n",
                 disagreeing, kPixels, locks_disagreeing, static_cast<double>(worst));
    CY_CHECK_LE(disagreeing, kPixels / 1000U);
    CY_CHECK_LE(locks_disagreeing, kPixels / 1000U);

    // The sharpened output against the host's sharpening of the device's own reconstruction.
    std::vector<Vec4> sharpened(kPixels);
    CY_REQUIRE(sharpen_reference(Span<const Vec4>(history.data(), history.size()),
                                 make_sharpen_constants(options.settings, kOutput),
                                 Span<Vec4>(sharpened.data(), sharpened.size()))
                   .has_value());
    usize sharpen_disagreeing = 0;
    for (usize at = 0; at < kPixels; ++at) {
        const f32 scale = std::fmax(std::fabs(sharpened[at].y), 1.0F);
        sharpen_disagreeing +=
            std::fabs(output[at].y - sharpened[at].y) > (scale * 4.0e-3F) + 0.5F ? 1U : 0U;
    }
    CY_CHECK_LE(sharpen_disagreeing, kPixels / 1000U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

// --- (c) and (d) -----------------------------------------------------------------------------

namespace {

/// The box's screen footprint in output pixels, conservatively padded.
struct Footprint {
    u32 left = 0;
    u32 right = 0;
    u32 top = 0;
    u32 bottom = 0;
};

[[nodiscard]] Footprint footprint(FrameScene& scene, Vec3 centre) noexcept {
    Footprint box{kWidth, 0, kHeight, 0};
    for (u32 corner = 0; corner < 8U; ++corner) {
        const Vec3 p{centre.x + (((corner & 1U) != 0U) ? kBoxHalf : -kBoxHalf),
                     centre.y + (((corner & 2U) != 0U) ? kBoxHalf : -kBoxHalf),
                     centre.z + (((corner & 4U) != 0U) ? kBoxHalf : -kBoxHalf)};
        const Vec4 clip = scene.projection() *
                          Vec4{p.x - scene.eye().x, p.y - scene.eye().y, p.z - scene.eye().z, 1.0F};
        const f32 x = (((clip.x / clip.w) * 0.5F) + 0.5F) * static_cast<f32>(kWidth);
        const f32 y = (0.5F - ((clip.y / clip.w) * 0.5F)) * static_cast<f32>(kHeight);
        box.left = std::min(box.left, static_cast<u32>(std::fmax(x - 2.0F, 0.0F)));
        box.right = std::max(box.right, std::min(static_cast<u32>(x + 3.0F), kWidth));
        box.top = std::min(box.top, static_cast<u32>(std::fmax(y - 2.0F, 0.0F)));
        box.bottom = std::max(box.bottom, std::min(static_cast<u32>(y + 3.0F), kHeight));
    }
    return box;
}

/// Pixels that are not the truth and lean toward `box`, the colour of the object that left: wrong
/// by more than `threshold` 8-bit steps in a channel AND nearer the box's colour than the truth is
/// by a margin. A freshly revealed edge reconstructed from one frame differs from a converged one
/// without leaning toward anything; a ghost leans toward the object it is a ghost of.
template <typename Inside>
[[nodiscard]] usize leaning(const std::vector<u32>& image, const std::vector<u32>& truth, u32 box,
                            u32 threshold, Inside inside) noexcept {
    const auto distance = [](u32 a, u32 b) noexcept {
        i32 worst = 0;
        for (u32 c = 0; c < 3U; ++c) {
            const auto da = static_cast<i32>(channel(a, c));
            const auto db = static_cast<i32>(channel(b, c));
            worst = std::max(worst, da > db ? da - db : db - da);
        }
        return worst;
    };
    usize count = 0;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            if (!inside(x, y)) {
                continue;
            }
            const usize at = (static_cast<usize>(y) * kWidth) + x;
            const bool wrong = distance(image[at], truth[at]) > static_cast<i32>(threshold);
            const bool toward = distance(image[at], box) + 16 < distance(truth[at], box);
            count += wrong && toward ? 1U : 0U;
        }
    }
    return count;
}

/// The colour of the middle of box 1's footprint in `image`.
[[nodiscard]] u32 box_colour(FrameScene& scene, const std::vector<u32>& image,
                             Vec3 centre) noexcept {
    const Footprint box = footprint(scene, centre);
    return image[(static_cast<usize>((box.top + box.bottom) / 2U) * kWidth) +
                 ((box.left + box.right) / 2U)];
}

/// What the loosened control turns off: rejection by depth, the shading-change test, and the
/// clip — history taken wherever the motion points.
[[nodiscard]] UpscaleSettings without_rejection(UpscaleMode mode) noexcept {
    UpscaleSettings settings = settings_for(mode);
    settings.reject_disocclusion = false;
    settings.detect_shading_change = false;
    settings.still_clip_gamma = 1000.0F;
    settings.moving_clip_gamma = 1000.0F;
    return settings;
}

constexpr f32 kSlide = 0.12F;
constexpr u32 kSlideFrames = 16;

/// Slide the box `kSlideFrames` frames and count the pixels behind it that are not the floor the
/// native frame shows there.
[[nodiscard]] usize trail(DeviceFixture& fixture, const UpscaleSettings& settings,
                          const std::vector<u32>& truth, Vec3 end, const char* picture) {
    RunOptions options;
    options.mode = UpscaleMode::Performance;
    options.settings = settings;
    Run run(fixture, options);
    if (!run.ready() || !run.frames(kConverge / 2U, kBoxStart)) {
        return kPixels;
    }
    for (u32 frame = 1; frame <= kSlideFrames; ++frame) {
        if (!run.frame(
                Vec3{kBoxStart.x + (kSlide * static_cast<f32>(frame)), kBoxStart.y, kBoxStart.z})) {
            return kPixels;
        }
    }
    save(picture, run.pixels());
    const Footprint now = footprint(run.scene(), end);
    const Footprint start = footprint(run.scene(), kBoxStart);
    const u32 box = box_colour(run.scene(), truth, end);
    // Behind the box: where it was over the slide and is not now.
    const auto behind = [&](u32 x, u32 y) {
        return x >= start.left && x < now.left && y >= now.top && y < now.bottom;
    };
    usize area = 0;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            area += behind(x, y) ? 1U : 0U;
        }
    }
    std::fprintf(stderr, "(c) %s: %zu pixels swept\n", picture, area);
    return leaning(run.pixels(), truth, box, 24, behind);
}

}  // namespace

CY_TEST_CASE("(c) a sliding box leaves no trail behind it") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    const Vec3 end{kBoxStart.x + (kSlide * static_cast<f32>(kSlideFrames)), kBoxStart.y,
                   kBoxStart.z};
    // THE TRUTH IS THE SAME MODE, STILL: the box converged where the slide ends. Against it a
    // static silhouette is the same picture in both, and what differs behind the box is the trail.
    std::vector<u32> truth;
    CY_REQUIRE(native_reference(fixture, truth, end, UpscaleMode::Performance));
    save("upscale-trail-truth.png", truth);
    const usize kept =
        trail(fixture, settings_for(UpscaleMode::Performance), truth, end, "upscale-trail.png");
    const usize ghosted = trail(fixture, without_rejection(UpscaleMode::Performance), truth, end,
                                "upscale-trail-control.png");
    std::fprintf(stderr, "(c) trail: %zu pixels, %zu without rejection\n", kept, ghosted);
    // Of about 3200 pixels swept: measured 58, against 2887 with rejection, the shading-change test
    // and the clip all off. What remains is the floor seam under the box and the edges of the boxes
    // behind it, revealed during the slide and a few frames short of converged.
    CY_CHECK_LT(kept, 80U);
    CY_CHECK_GT(ghosted, kept * 20U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(d) a box that jumps away reveals the floor rebuilt from this frame") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    const Vec3 away{kBoxStart.x + 2.5F, kBoxStart.y, kBoxStart.z};
    std::vector<u32> truth;
    CY_REQUIRE(native_reference(fixture, truth, away, UpscaleMode::Performance));
    save("upscale-revealed-truth.png", truth);
    usize revealed[2] = {};
    for (u32 pass = 0; pass < 2U; ++pass) {
        RunOptions options;
        options.mode = UpscaleMode::Performance;
        options.settings = pass == 0U ? settings_for(UpscaleMode::Performance)
                                      : without_rejection(UpscaleMode::Performance);
        Run run(fixture, options);
        CY_REQUIRE(run.ready());
        CY_REQUIRE(run.frames(kConverge / 2U, kBoxStart));
        // ONE frame after the jump: the floor where the box stood has no history of its own.
        CY_REQUIRE(run.frame(away));
        save(pass == 0U ? "upscale-revealed.png" : "upscale-revealed-control.png", run.pixels());
        const Footprint before = footprint(run.scene(), kBoxStart);
        const Footprint after = footprint(run.scene(), away);
        const u32 box = box_colour(run.scene(), truth, away);
        revealed[pass] = leaning(run.pixels(), truth, box, 24, [&](u32 x, u32 y) {
            const bool was =
                x >= before.left && x < before.right && y >= before.top && y < before.bottom;
            const bool is =
                x >= after.left && x < after.right && y >= after.top && y < after.bottom;
            return was && !is;
        });
    }
    std::fprintf(stderr, "(d) revealed: %zu pixels wrong, %zu without rejection\n", revealed[0],
                 revealed[1]);
    // Measured 120 — the one-pixel line where the box stood on the floor, at the floor's own depth,
    // and the edges of the blue box behind it, reconstructed from one frame — against 4160 with
    // rejection, the shading-change test and the clip all off.
    CY_CHECK_LT(revealed[0], 160U);
    CY_CHECK_GT(revealed[1], revealed[0] * 20U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

// --- (e) ---------------------------------------------------------------------------------------

CY_TEST_CASE("(e) the interface is drawn at the output extent, the same bytes in every mode") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // Where the interface covers the picture: the panel, the label's glyphs on it, and the second
    // panel — on pixel boundaries and off them (the second is 61 by 37 pixels from (300, 200)).
    const auto covered = [](u32 x, u32 y) {
        return (x >= 12 && x < 162 && y >= 12 && y < 52) ||
               (x >= 300 && x < 361 && y >= 200 && y < 237);
    };
    std::vector<u32> native;
    {
        RunOptions options;
        options.upscale = false;
        options.interface = true;
        Run run(fixture, options);
        CY_REQUIRE(run.ready());
        CY_REQUIRE(run.frames(4));
        native = run.pixels();
        save("upscale-interface-native.png", native);
    }
    for (const UpscaleMode mode : {UpscaleMode::Quality, UpscaleMode::Performance}) {
        RunOptions options;
        options.mode = mode;
        options.settings = settings_for(mode);
        options.settings.sharpness = 0.5F;
        options.interface = true;
        Run run(fixture, options);
        CY_REQUIRE(run.ready());
        CY_REQUIRE(run.frames(4));
        usize interface_pixels = 0;
        usize differing_pixels = 0;
        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = 0; x < kWidth; ++x) {
                if (!covered(x, y)) {
                    continue;
                }
                const usize at = (static_cast<usize>(y) * kWidth) + x;
                ++interface_pixels;
                differing_pixels += run.pixels()[at] != native[at] ? 1U : 0U;
            }
        }
        std::fprintf(stderr, "(e) %-12s %zu interface pixels, %zu differ from native\n",
                     upscale_mode_name(mode), interface_pixels, differing_pixels);
        CY_CHECK_EQ(differing_pixels, 0U);
        // And the panels are the colours the interface asked for, byte for byte.
        const u32 panel = run.pixels()[(static_cast<usize>(14) * kWidth) + 160];
        CY_CHECK_EQ(panel, ((kPanelColour >> 16U) & 0xFFU) | (kPanelColour & 0xFF00U) |
                               ((kPanelColour & 0xFFU) << 16U) | 0xFF000000U);
        if (mode == UpscaleMode::Performance) {
            save("upscale-interface-performance.png", run.pixels());
        }
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

// --- (f) ---------------------------------------------------------------------------------------

CY_TEST_CASE("(f) the budget arbiter moves the render extent and the history survives it") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // The arbiter with one registered subsystem already at its minimum, so that a frame over
    // budget reaches the only lever left — resolution scale — on the arbiter's own period.
    BudgetArbiter arbiter;
    ArbiterConfig config;
    config.period_frames = 4;
    CY_REQUIRE(arbiter.configure(config).has_value());
    SubsystemDeclaration post;
    post.subsystem = BudgetSubsystem::PostProcessing;
    post.ladder.positions = 1;
    post.ladder.relative_cost[0] = 1.0F;
    CY_REQUIRE(arbiter.declare(post).has_value());

    std::vector<u32> native;
    CY_REQUIRE(native_reference(fixture, native));
    RunOptions options;
    options.mode = UpscaleMode::Quality;
    options.settings = settings_for(UpscaleMode::Quality);
    Run run(fixture, options);
    CY_REQUIRE(run.ready());
    const DynamicResolutionBounds bounds;
    // The extent the arbiter's authored allocation gives: the mode's, snapped to the granule.
    const Extent2D authored = dynamic_render_extent(kOutput, UpscaleMode::Quality, 1.0F, bounds);
    CY_REQUIRE(run.set_render(authored));
    CY_REQUIRE(run.frames(kConverge / 2U));
    const u64 resets_before = run.pass().report().history_resets;
    Extent2D lowest = authored;
    // Over budget: the GPU frame measures 18 ms against 13.9.
    for (u32 frame = 0; frame < 24U; ++frame) {
        arbiter.report_frame_ms(18.0F);
        arbiter.report_subsystem(BudgetSubsystem::PostProcessing, 2.0F, 0, true,
                                 CostSource::Measured);
        (void)arbiter.update();
        const Extent2D extent = dynamic_render_extent(kOutput, UpscaleMode::Quality,
                                                      arbiter.resolution_scale(), bounds);
        if (!(extent == run.render())) {
            CY_REQUIRE(run.set_render(extent));
        }
        lowest = extent.width < lowest.width ? extent : lowest;
        CY_REQUIRE(run.frame());
        // NOT A CUT: the framework's view is the output, which never moved.
        CY_CHECK_EQ(run.report().temporal_cause, TemporalInvalidation::None);
    }
    std::fprintf(stderr, "(f) render extent %ux%u -> %ux%u, arbiter scale %.2f, %u changes\n",
                 authored.width, authored.height, lowest.width, lowest.height,
                 static_cast<double>(arbiter.resolution_scale()),
                 run.pass().report().render_changes);
    CY_CHECK_LT(lowest.width, authored.width);
    CY_CHECK_GE(static_cast<f32>(lowest.width),
                (bounds.minimum_scale * static_cast<f32>(kWidth)) - 8.0F);
    CY_CHECK_GT(run.pass().report().render_changes, 0U);
    // The history was never reset across the moves.
    CY_CHECK_EQ(run.pass().report().history_resets, resets_before);

    // Headroom returns: the arbiter raises the allocation again.
    for (u32 frame = 0; frame < 120U && !(run.render() == authored); ++frame) {
        arbiter.report_frame_ms(6.0F);
        arbiter.report_subsystem(BudgetSubsystem::PostProcessing, 1.0F, 0, true,
                                 CostSource::Measured);
        (void)arbiter.update();
        const Extent2D extent = dynamic_render_extent(kOutput, UpscaleMode::Quality,
                                                      arbiter.resolution_scale(), bounds);
        if (!(extent == run.render())) {
            CY_REQUIRE(run.set_render(extent));
        }
        CY_REQUIRE(run.frame());
    }
    CY_CHECK(run.render() == authored);
    // And the picture is still the converged one, not a reset.
    CY_REQUIRE(run.frames(8));
    const f64 error = mean_error(run.pixels(), native);
    std::fprintf(stderr, "(f) after the round trip: %.3f (8-bit) from native\n", error);
    CY_CHECK_LT(error, 1.5);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

// --- (g) ---------------------------------------------------------------------------------------

CY_TEST_CASE("(g) motion blur after the upscaler reads the depth and motion at the output extent") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // Native TAA and Performance, each with motion blur and without it. What the blur MOVED in
    // each is the difference between the two, so the comparison is of streaks and not of two
    // reconstructions of a moving edge, which differ by the upscaler's own error.
    std::vector<u32> pictures[4];
    for (u32 index = 0; index < 4U; ++index) {
        RunOptions options;
        options.upscale = (index & 1U) != 0U;
        options.mode = UpscaleMode::Performance;
        options.settings = settings_for(UpscaleMode::Performance);
        options.motion_blur = index < 2U;
        Run run(fixture, options);
        CY_REQUIRE(run.ready());
        for (u32 frame = 0; frame < 12U; ++frame) {
            CY_REQUIRE(run.frame(
                Vec3{kBoxStart.x + (0.3F * static_cast<f32>(frame)), kBoxStart.y, kBoxStart.z}));
        }
        if (options.upscale && options.motion_blur) {
            CY_CHECK(run.pass().report().resampled);
        }
        pictures[index] = run.pixels();
    }
    save("upscale-motion-blur-native.png", pictures[0]);
    save("upscale-motion-blur-performance.png", pictures[1]);
    const auto moved = [&](u32 blurred, u32 x, u32 y) {
        const usize at = (static_cast<usize>(y) * kWidth) + x;
        for (u32 c = 0; c < 3U; ++c) {
            const auto a = static_cast<i32>(channel(pictures[blurred][at], c));
            const auto b = static_cast<i32>(channel(pictures[blurred + 2U][at], c));
            if ((a > b ? a - b : b - a) > 6) {
                return true;
            }
        }
        return false;
    };
    usize native_streak = 0;
    usize upscaled_streak = 0;
    usize both = 0;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const bool native = moved(0U, x, y);
            const bool upscaled = moved(1U, x, y);
            native_streak += native ? 1U : 0U;
            upscaled_streak += upscaled ? 1U : 0U;
            both += native && upscaled ? 1U : 0U;
        }
    }
    const usize either = native_streak + upscaled_streak - both;
    const f64 overlap = either == 0U ? 0.0 : static_cast<f64>(both) / static_cast<f64>(either);
    const f64 error = mean_error(pictures[1], pictures[0]);
    std::fprintf(stderr,
                 "(g) motion blur, performance against native TAA: %.3f (8-bit); the blur moved "
                 "%zu pixels native and %zu upscaled, %.2f of them the same\n",
                 error, native_streak, upscaled_streak, overlap);
    CY_CHECK_LT(error, 4.0);
    // The blur moved the same pixels at a quarter of the shaded ones: a blur reading depth or
    // motion at the render extent loads them at the wrong texels and moves others, or none.
    CY_REQUIRE(native_streak > 200U);
    CY_CHECK_GT(overlap, 0.4);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
