// SPDX-License-Identifier: MIT
// Per-object motion vectors and motion blur on a Vulkan device, with validation and
// synchronisation validation on. `render.motion_blur`.
//
// ================================================================================================
// THE SCENE
// ================================================================================================
//
// `pipeline_test::FrameScene` — the one scene the pipeline suites render — with its ring of boxes
// rearranged: box 1, a metre wide, rests on the floor five metres ahead and slides along +x by
// `kStep` metres a frame; boxes 2 to 11 stand still in a row four metres behind it. Nothing tells
// the frame which box moves. Its instance row changes between two renders and that is all, which
// is exactly what a game's frame sees.
//
// ================================================================================================
// THE PROPERTIES, EACH A CASE
// ================================================================================================
//
//   (a) the moving box's velocity is its screen displacement, and the still background's is the
//       camera's alone: every pixel's motion vector against the reprojection of the surface the
//       depth buffer holds there, the box's through its own displacement and everything else's
//       through the camera's — with the camera still and with the camera moving;
//   (b) temporal antialiasing ghosts less behind the moving box than with camera motion only,
//       against the same frame resolved with no history;
//   (c) the blur's streak follows the shutter angle — half a frame of motion at 180 degrees, a
//       whole one at 360 — and the background beyond the streak is untouched, and the device's
//       gather is the host reference's;
//   (d) a closed shutter is the frame without the stage, byte for byte;
//   (e) with neither per-object motion nor the stage, the frame is byte-identical to committed
//       references drawn with the frame shaders from before per-object motion — a moving box, and
//       a still scene with per-object motion ON, which is what "temporal antialiasing of a static
//       scene is unchanged" means.

#include "frame_scene.h"
#include "golden.h"
#include "motion_measure.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/motion_blur/motion_blur_pass.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace cy;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
using namespace cy::rendering::motion_blur;

namespace {

constexpr usize kPixels = static_cast<usize>(kWidth) * kHeight;
constexpr f32 kFloorTop = -1.9F + 0.125F;
constexpr f32 kBoxHalf = 0.5F;
/// Where the moving box starts, and how far it slides along +x each frame: about 25 pixels at five
/// metres, fast enough that a streak is many pixels long and slow enough that it stays in view.
constexpr Vec3 kBoxStart{-1.6F, kFloorTop + kBoxHalf, -5.0F};
constexpr f32 kStep = 0.45F;
/// Frames rendered before the one a case measures, so the temporal history has converged.
constexpr u32 kFrames = 5;

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
        description.application_name = "cy_test_render_motion_blur";
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

/// `references/<name>`: this scene rendered by the tree as it was before per-object motion — the
/// suite's (e) run once with `CY_RENDER_UPDATE_GOLDEN=1` against main's frame shaders, before the
/// regenerated `frame_spirv.h` was built.
const char* reference_path(const char* name) noexcept {
    static char storage[1024];
    (void)std::snprintf(storage, sizeof(storage), "%s/references/%s", CY_MOTION_BLUR_TEST_DIR,
                        name);
    return storage;
}

bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/// The field: box 1 in front, sliding; the rest a still row behind it.
void place_box(u32 which, Vec3& centre, f32& half, void* /*user*/) noexcept {
    if (which == 1U) {
        centre = kBoxStart;
        half = kBoxHalf;
        return;
    }
    half = 0.3F + (0.06F * static_cast<f32>(which % 3U));
    centre = Vec3{-4.5F + static_cast<f32>(which - 2U), kFloorTop + half, -9.0F};
}

// --- One run: a scene, the stage, and what they read back ------------------------------------

struct RunOptions {
    /// The stage in the frame at all. Off is the frame without motion blur.
    bool motion_blur = false;
    MotionBlurSettings settings{};
    /// `FrameUpload::object_motion`. Off is camera motion only — the frame from before.
    bool object_motion = true;
    /// Where the pinned jitter sequence starts.
    u32 jitter_index = 0;
};

struct RunState;

void configure_run(rendering::assembly::AssemblyDescription& description, void* user) noexcept;
Status before_assemble(rendering::RenderGraph& graph, rendering::assembly::AssemblyView& view,
                       rendering::assembly::FrameSinks& sinks, void* user) noexcept;
Status before_upload(rendering::pipeline::FrameUpload& upload, void* user) noexcept;

struct RunState {
    FrameScene* scene = nullptr;
    MotionBlurPass* pass = nullptr;
    const RunOptions* options = nullptr;
    Vec3 previous_eye{0.0F, 0.0F, 0.0F};
    Status view_status;
};

/// The motion blur view of this frame: the scene's projection, and last frame's camera expressed
/// about this frame's — the scene looks down -Z with no rotation, so that is the camera's
/// displacement and nothing else.
[[nodiscard]] MotionBlurView blur_view(const FrameScene& scene, Vec3 previous_eye) noexcept {
    MotionBlurView view;
    view.width = kWidth;
    view.height = kHeight;
    view.projection = scene.projection();
    view.relative_to_clip = scene.projection();
    view.previous_relative_to_clip =
        scene.projection() * Mat4::from_translation(scene.eye() - previous_eye);
    return view;
}

void configure_run(rendering::assembly::AssemblyDescription& description, void* user) noexcept {
    const auto* run = static_cast<const RunState*>(user);
    description.pin_jitter = true;
    description.pinned_jitter_index = run->options->jitter_index;
    description.post.motion_blur = run->options->motion_blur;
}

Status before_assemble(rendering::RenderGraph& graph, rendering::assembly::AssemblyView& view,
                       rendering::assembly::FrameSinks& sinks, void* user) noexcept {
    auto* run = static_cast<RunState*>(user);
    if (!run->options->motion_blur) {
        return ok();
    }
    run->pass->set_settings(run->options->settings);
    if (Status set = run->pass->set_view(blur_view(*run->scene, run->previous_eye)); !set) {
        return set;
    }
    view.motion_blur = run->pass->import_target(graph);
    sinks.motion_blur = run->pass->stage();
    return ok();
}

Status before_upload(rendering::pipeline::FrameUpload& upload, void* user) noexcept {
    const auto* run = static_cast<const RunState*>(user);
    upload.object_motion = run->options->object_motion;
    return ok();
}

class MotionRun {
public:
    MotionRun(DeviceFixture& fixture, const RunOptions& options)
        : device_(&fixture.device()), options_(options), scene_(allocator()) {
        state_ = RunState{&scene_, &pass_, &options_, Vec3{0.0F, 0.0F, 0.0F}, ok()};
        FrameSceneHooks hooks;
        hooks.user = &state_;
        hooks.configure = &configure_run;
        hooks.before_assemble = &before_assemble;
        hooks.before_upload = &before_upload;
        hooks.place_box = &place_box;
        scene_.set_hooks(hooks);
        if (const Status built = scene_.build(fixture.device()); !built) {
            std::fprintf(stderr, "motion frame: build failed: %s\n", built.error().message);
            return;
        }
        scene_.set_read_back(true);
        if (options_.motion_blur) {
            MotionBlurPassDescription description;
            description.width = kWidth;
            description.height = kHeight;
            description.readback = true;
            if (const Status created = pass_.create(allocator(), fixture.device(), description);
                !created) {
                std::fprintf(stderr, "motion frame: pass failed: %s\n", created.error().message);
                return;
            }
        }
        ready_ = true;
    }
    ~MotionRun() {
        (void)device_->wait_idle();
        pass_.destroy();
        scene_.release();
    }
    MotionRun(const MotionRun&) = delete;
    MotionRun& operator=(const MotionRun&) = delete;

    /// Render one frame with the box at `centre` and the camera at `eye`.
    [[nodiscard]] bool frame(Vec3 centre, Vec3 eye = Vec3{0.0F, 0.0F, 0.0F}) {
        if (!ready_ || !scene_.move_box(1, centre).has_value()) {
            return false;
        }
        state_.previous_eye = scene_.eye();
        scene_.set_eye(eye);
        const Status rendered = scene_.render(RecordMode::Callbacks, report_);
        if (!rendered) {
            std::fprintf(stderr, "motion frame: render failed: %s\n", rendered.error().message);
            return false;
        }
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        box_ = centre;
        if (!options_.motion_blur) {
            return true;
        }
        input_.assign(kPixels, Vec4{});
        output_.assign(kPixels, Vec4{});
        velocity_.assign(kPixels, Vec2{});
        depth_.assign(kPixels, 0.0F);
        MotionBlurReadback out;
        out.input = Span<Vec4>(input_.data(), input_.size());
        out.output = Span<Vec4>(output_.data(), output_.size());
        out.velocity = Span<Vec2>(velocity_.data(), velocity_.size());
        out.depth = Span<f32>(depth_.data(), depth_.size());
        return pass_.read_back(out).has_value();
    }

    /// `kFrames` frames of the box sliding from its start, the camera following `pan` metres a
    /// frame.
    [[nodiscard]] bool slide(Vec3 pan = Vec3{0.0F, 0.0F, 0.0F}, u32 frames = kFrames) {
        for (u32 index = 0; index < frames; ++index) {
            const f32 step = static_cast<f32>(index);
            const Vec3 centre{kBoxStart.x + (kStep * step), kBoxStart.y, kBoxStart.z};
            if (!frame(centre, Vec3{pan.x * step, pan.y * step, pan.z * step})) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] const std::vector<u32>& pixels() const { return pixels_; }
    [[nodiscard]] const std::vector<Vec4>& input() const { return input_; }
    [[nodiscard]] const std::vector<Vec4>& output() const { return output_; }
    [[nodiscard]] const std::vector<Vec2>& velocity() const { return velocity_; }
    [[nodiscard]] const std::vector<f32>& depth() const { return depth_; }
    [[nodiscard]] const MotionBlurPass& pass() const { return pass_; }
    [[nodiscard]] FrameScene& scene() { return scene_; }
    [[nodiscard]] Vec3 box() const { return box_; }
    [[nodiscard]] const rendering::assembly::AssemblyReport& report() const { return report_; }

private:
    rhi::Device* device_ = nullptr;
    RunOptions options_;
    FrameScene scene_;
    MotionBlurPass pass_;
    RunState state_{};
    rendering::assembly::AssemblyReport report_{};
    bool ready_ = false;
    std::vector<u32> pixels_;
    std::vector<Vec4> input_;
    std::vector<Vec4> output_;
    std::vector<Vec2> velocity_;
    std::vector<f32> depth_;
    Vec3 box_{};
};

// --- The geometry's answers ----------------------------------------------------------------------

/// The WORLD point the depth buffer holds at pixel (x, y), for a frame whose projection was
/// jittered by `jitter` pixels. The rasteriser sampled the pixel centre through the JITTERED
/// projection, so the surface there projects through the unjittered one to the centre minus the
/// jitter — which is also the position the prepass's jitter-free motion vector starts from.
struct Surface {
    Vec3 world{};
    Vec2 ndc{};
};

[[nodiscard]] Surface surface_at(const FrameScene& scene, const Mat4& inverse_clip, Vec2 jitter,
                                 u32 x, u32 y, f32 depth) noexcept {
    const f32 u = (static_cast<f32>(x) + 0.5F) / static_cast<f32>(kWidth);
    const f32 v = (static_cast<f32>(y) + 0.5F) / static_cast<f32>(kHeight);
    Surface surface;
    surface.ndc = Vec2{(u * 2.0F) - 1.0F - (jitter.x * 2.0F / static_cast<f32>(kWidth)),
                       1.0F - (v * 2.0F) - (jitter.y * 2.0F / static_cast<f32>(kHeight))};
    const Vec4 relative = inverse_clip * Vec4{surface.ndc.x, surface.ndc.y, depth, 1.0F};
    surface.world =
        Vec3{(relative.x / relative.w) + scene.eye().x, (relative.y / relative.w) + scene.eye().y,
             (relative.z / relative.w) + scene.eye().z};
    return surface;
}

/// The motion vector the prepass should write for a surface that was at `previous_world` last
/// frame, seen last frame from `previous_eye`: normalised screen space, pointing at the past.
[[nodiscard]] Vec2 expected_motion(const FrameScene& scene, const Surface& surface,
                                   Vec3 previous_world, Vec3 previous_eye) noexcept {
    const Vec3 relative = previous_world - previous_eye;
    const Vec4 clip = scene.projection() * Vec4{relative.x, relative.y, relative.z, 1.0F};
    const Vec2 previous{clip.x / clip.w, clip.y / clip.w};
    return Vec2{(previous.x - surface.ndc.x) * 0.5F, (surface.ndc.y - previous.y) * 0.5F};
}

[[nodiscard]] bool inside_box(Vec3 point, Vec3 centre, f32 slack) noexcept {
    return std::fabs(point.x - centre.x) <= kBoxHalf + slack &&
           std::fabs(point.y - centre.y) <= kBoxHalf + slack &&
           std::fabs(point.z - centre.z) <= kBoxHalf + slack;
}

struct VelocityCheck {
    u32 box_pixels = 0;
    u32 still_pixels = 0;
    /// Texels on the line where the box meets the floor, which are either surface — not checked.
    u32 contact_pixels = 0;
    /// Worst disagreement, in pixels, between the written motion and the reprojection.
    f32 worst_box = 0.0F;
    f32 worst_still = 0.0F;
    /// How far the box's own motion is from what camera motion alone would have written there —
    /// what makes the box's agreement a measurement of per-object motion rather than of the camera.
    f32 box_object_share = 0.0F;
};

/// Every covered pixel's motion vector against the reprojection of the surface under it.
[[nodiscard]] VelocityCheck check_velocity(MotionRun& run, Vec3 previous_box, Vec3 previous_eye) {
    FrameScene& scene = run.scene();
    const Expected<Mat4, Error> inverse_clip = inverse(scene.projection());
    CY_REQUIRE(inverse_clip.has_value());
    const Vec2 jitter = run.report().jitter;
    const Vec3 displacement = run.box() - previous_box;
    VelocityCheck check;
    f32 share = 0.0F;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const usize at = (static_cast<usize>(y) * kWidth) + x;
            const f32 depth = run.depth()[at];
            if (depth <= 0.0F) {
                continue;
            }
            const Surface surface = surface_at(scene, *inverse_clip, jitter, x, y, depth);
            const Vec2 still = expected_motion(scene, surface, surface.world, previous_eye);
            const Vec2 written = run.velocity()[at];
            // THE LINE WHERE THE BOX MEETS THE FLOOR IS BOTH: the box stands on the floor, so a
            // texel whose surface point is within the slack of the floor's plane AND of the box
            // is the box's foot or the floor beside it, and its depth cannot say which. Neither
            // answer is checked there; every other texel is.
            const bool on_box = inside_box(surface.world, run.box(), 1e-3F);
            if (on_box && std::fabs(surface.world.y - kFloorTop) <= 1e-3F) {
                ++check.contact_pixels;
                continue;
            }
            const Vec2 expected =
                on_box ? expected_motion(scene, surface, surface.world - displacement, previous_eye)
                       : still;
            const f32 error = std::hypot((written.x - expected.x) * static_cast<f32>(kWidth),
                                         (written.y - expected.y) * static_cast<f32>(kHeight));
            if (on_box) {
                ++check.box_pixels;
                check.worst_box = std::max(check.worst_box, error);
                share += std::hypot((expected.x - still.x) * static_cast<f32>(kWidth),
                                    (expected.y - still.y) * static_cast<f32>(kHeight));
            } else {
                ++check.still_pixels;
                check.worst_still = std::max(check.worst_still, error);
            }
        }
    }
    check.box_object_share =
        check.box_pixels == 0 ? 0.0F : share / static_cast<f32>(check.box_pixels);
    return check;
}

/// The half-float precision a motion vector is stored at, in pixels, for motions this size: a
/// normalised coordinate near 0.05 has eleven bits of mantissa, a few thousandths of a pixel, and
/// the tolerance is ten times that.
constexpr f32 kVelocityTolerancePixels = 0.03F;

[[nodiscard]] f32 luminance_of(const Vec4& colour) noexcept {
    return motion_test::luminance(colour.x, colour.y, colour.z);
}

/// The columns of row `y` the box covers in the unblurred colour, from the depth: the leftmost and
/// one past the rightmost.
struct Span2 {
    u32 first = 0;
    u32 last = 0;
};

[[nodiscard]] Span2 box_columns(MotionRun& run, u32 y) {
    FrameScene& scene = run.scene();
    const Expected<Mat4, Error> inverse_clip = inverse(scene.projection());
    CY_REQUIRE(inverse_clip.has_value());
    Span2 columns{kWidth, 0};
    for (u32 x = 0; x < kWidth; ++x) {
        const usize at = (static_cast<usize>(y) * kWidth) + x;
        const f32 depth = run.depth()[at];
        if (depth <= 0.0F) {
            continue;
        }
        const Surface surface = surface_at(scene, *inverse_clip, run.report().jitter, x, y, depth);
        if (inside_box(surface.world, run.box(), 1e-3F)) {
            columns.first = std::min(columns.first, x);
            columns.last = std::max(columns.last, x + 1U);
        }
    }
    return columns;
}

/// The row through the middle of the box's screen footprint.
[[nodiscard]] u32 box_row(const FrameScene& scene, Vec3 centre) noexcept {
    const Vec4 clip = scene.projection() * Vec4{centre.x - scene.eye().x, centre.y - scene.eye().y,
                                                centre.z + kBoxHalf - scene.eye().z, 1.0F};
    return static_cast<u32>((0.5F - (clip.y / clip.w) * 0.5F) * static_cast<f32>(kHeight));
}

[[nodiscard]] usize differing(const std::vector<u32>& a, const std::vector<u32>& b) noexcept {
    usize count = 0;
    for (usize index = 0; index < a.size() && index < b.size(); ++index) {
        count += static_cast<usize>(a[index] != b[index]);
    }
    return count + (a.size() > b.size() ? a.size() - b.size() : b.size() - a.size());
}

[[nodiscard]] MotionBlurSettings shutter(f32 degrees) noexcept {
    MotionBlurSettings settings;
    settings.shutter_angle_degrees = degrees;
    return settings;
}

/// Compare a frame against a committed reference, or — under `CY_RENDER_UPDATE_GOLDEN` — write it.
void check_against_reference(const char* name, const std::vector<u32>& pixels) {
    render_test::Image rendered(allocator());
    CY_REQUIRE(
        render_test::adopt(rendered, Span<const u32>(pixels.data(), pixels.size()), kWidth, kHeight)
            .has_value());
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

}  // namespace

CY_TEST_CASE("(e) without per-object motion or the stage, the frame is the frame from before") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // A moving box, drawn as every frame before this change drew it: camera motion only.
    RunOptions before;
    before.object_motion = false;
    MotionRun moving(fixture, before);
    CY_REQUIRE(moving.slide());
    save("motion-off-moving.png", moving.pixels());
    check_against_reference("motion_off_moving.png", moving.pixels());

    // A STILL scene with per-object motion ON — the default. Every previous row is its current row
    // bit for bit, so the velocity, and the temporal resolve that reads it, are the frame's before.
    RunOptions still;
    MotionRun resting(fixture, still);
    for (u32 index = 0; index < kFrames; ++index) {
        CY_REQUIRE(resting.frame(kBoxStart));
    }
    save("motion-static-taa.png", resting.pixels());
    check_against_reference("motion_static_taa.png", resting.pixels());
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(a) a moving box's velocity is its screen displacement, the rest the camera's") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // Shutter zero: the stage copies its input, and it is here for its readback of the prepass's
    // velocity and depth.
    RunOptions options;
    options.motion_blur = true;
    options.settings = shutter(0.0F);

    const Vec3 pans[2] = {Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.08F, 0.03F, 0.0F}};
    for (const Vec3 pan : pans) {
        MotionRun run(fixture, options);
        CY_REQUIRE(run.slide(pan, 2));
        const Vec3 previous_box{kBoxStart.x, kBoxStart.y, kBoxStart.z};
        const VelocityCheck check = check_velocity(run, previous_box, Vec3{0.0F, 0.0F, 0.0F});
        std::fprintf(stderr,
                     "(a) camera pan (%.2f, %.2f): box %u px worst %.4f px (object share %.2f px), "
                     "still %u px worst %.4f px\n",
                     static_cast<double>(pan.x), static_cast<double>(pan.y), check.box_pixels,
                     static_cast<double>(check.worst_box),
                     static_cast<double>(check.box_object_share), check.still_pixels,
                     static_cast<double>(check.worst_still));
        CY_CHECK_GT(check.box_pixels, 1500U);
        CY_CHECK_GT(check.still_pixels, 20000U);
        // The box's foot is one row of its front face, not a region the check can hide in.
        std::fprintf(stderr, "(a) box/floor contact texels not checked: %u\n",
                     check.contact_pixels);
        CY_CHECK_LT(check.contact_pixels, 16U);
        CY_CHECK_LT(check.worst_box, kVelocityTolerancePixels);
        CY_CHECK_LT(check.worst_still, kVelocityTolerancePixels);
        // The box moved about 25 pixels: its motion is its own, not the camera's.
        CY_CHECK_GT(check.box_object_share, 20.0F);
    }

    // THE CONTROL: camera motion only writes the still world's motion on the box too, so the same
    // check finds the box's pixels a whole frame of motion out.
    RunOptions camera_only = options;
    camera_only.object_motion = false;
    MotionRun control(fixture, camera_only);
    CY_REQUIRE(control.slide(Vec3{}, 2));
    const VelocityCheck without = check_velocity(control, kBoxStart, Vec3{0.0F, 0.0F, 0.0F});
    CY_CHECK_GT(without.worst_box, 20.0F);
    CY_CHECK_LT(without.worst_still, kVelocityTolerancePixels);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(b) temporal antialiasing ghosts less than with camera motion only") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    RunOptions options;
    options.motion_blur = true;
    options.settings = shutter(0.0F);
    MotionRun object(fixture, options);
    CY_REQUIRE(object.slide());
    RunOptions camera = options;
    camera.object_motion = false;
    MotionRun camera_only(fixture, camera);
    CY_REQUIRE(camera_only.slide());
    CY_REQUIRE(object.report().jitter_index == camera_only.report().jitter_index);

    // THE TRUTH: the last frame alone, resolved with no history, from the same jitter sample.
    RunOptions truth_options = options;
    truth_options.jitter_index = object.report().jitter_index;
    MotionRun truth(fixture, truth_options);
    CY_REQUIRE(truth.frame(object.box()));
    CY_REQUIRE(truth.report().jitter_index == object.report().jitter_index);

    // Where the ghost can be: the rows of the box and every column it crossed in the last three
    // frames, plus a margin a neighbourhood clamp can leave a trail in.
    const u32 row = box_row(object.scene(), object.box());
    const Span2 now = box_columns(truth, row);
    const u32 trail = static_cast<u32>(3.0F * 25.0F);
    const u32 first = now.first > trail ? now.first - trail : 0U;
    const u32 last = std::min(now.last + 4U, kWidth);
    f64 object_error = 0.0;
    f64 camera_error = 0.0;
    f64 energy = 0.0;
    for (u32 y = row > 40U ? row - 40U : 0U; y < std::min(row + 40U, kHeight); ++y) {
        for (u32 x = first; x < last; ++x) {
            const usize at = (static_cast<usize>(y) * kWidth) + x;
            const f32 truth_luminance = luminance_of(truth.input()[at]);
            object_error +=
                static_cast<f64>(std::fabs(luminance_of(object.input()[at]) - truth_luminance));
            camera_error += static_cast<f64>(
                std::fabs(luminance_of(camera_only.input()[at]) - truth_luminance));
            energy += static_cast<f64>(truth_luminance);
        }
    }
    save("taa-object-motion.png", object.pixels());
    save("taa-camera-motion-only.png", camera_only.pixels());
    save("taa-no-history.png", truth.pixels());
    std::fprintf(stderr,
                 "(b) ghost energy against the frame with no history: per-object %.4f, camera "
                 "only %.4f (of %.1f)\n",
                 object_error / energy, camera_error / energy, energy);
    // LESS, AND BY A MEASURED MARGIN rather than a guessed one. The scene is flat-shaded, so the
    // resolve's neighbourhood clamp already removes most of camera-only motion's ghost, and what
    // is left in either run is mostly the edges of surfaces the box uncovered, whose history is
    // new. Measured on Vulkan: per-object 0.0080 against camera-only 0.0122 of the energy, a ratio
    // of 0.66. Per-object motion switched off in the prepass makes the two runs the same frame,
    // a ratio of 1.
    CY_CHECK_GT(camera_error, 0.0);
    CY_CHECK_LT(object_error, 0.8 * camera_error);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) the streak follows the shutter angle and the background behind it stays sharp") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    const f32 angles[2] = {180.0F, 360.0F};
    f32 lengths[2] = {};
    for (u32 which = 0; which < 2U; ++which) {
        RunOptions options;
        options.motion_blur = true;
        options.settings = shutter(angles[which]);
        MotionRun run(fixture, options);
        CY_REQUIRE(run.slide());

        // THE MOTION, MEASURED: the box's own velocity at its centre, in pixels.
        const u32 row = box_row(run.scene(), run.box());
        const Span2 columns = box_columns(run, row);
        CY_REQUIRE(columns.last > columns.first + 30U);
        const usize centre =
            (static_cast<usize>(row) * kWidth) + ((columns.first + columns.last) / 2U);
        const f32 motion = std::fabs(run.velocity()[centre].x) * static_cast<f32>(kWidth);

        // THE STREAK, MEASURED off the row at the box's leading edge: how far past it the blur
        // reaches onto the still background — half the shutter-open motion for a physical shutter
        // (`motion_measure.h` says why this and not the edge's ramp).
        std::vector<f32> unblurred(kWidth);
        std::vector<f32> blurred(kWidth);
        for (u32 x = 0; x < kWidth; ++x) {
            const usize at = (static_cast<usize>(row) * kWidth) + x;
            unblurred[x] = luminance_of(run.input()[at]);
            blurred[x] = luminance_of(run.output()[at]);
        }
        const f32 radius = motion * angles[which] / 720.0F;
        const u32 reach = static_cast<u32>(std::ceil(radius)) + 4U;
        lengths[which] = motion_test::smear_reach(unblurred, blurred, columns.last, reach,
                                                  unblurred[columns.last - reach], 0.01F);
        std::fprintf(stderr,
                     "(c) shutter %.0f: motion %.2f px, the blur reaches %.1f px past the edge, "
                     "of %.2f\n",
                     static_cast<double>(angles[which]), static_cast<double>(motion),
                     static_cast<double>(lengths[which]), static_cast<double>(radius));
        CY_CHECK_GT(motion, 20.0F);
        // Never farther than the shutter lets it, and short of it only by the cone's last
        // percent. Measured 5 of 7.0 and 11 of 14.0.
        CY_CHECK_LE(lengths[which], radius + 0.5F);
        CY_CHECK_GE(lengths[which], 0.6F * radius);

        // THE BACKGROUND BEYOND THE STREAK — past the box's leading edge by more than half the
        // shutter-open motion, and behind its trailing edge by the same — is its unblurred self.
        const u32 clear = static_cast<u32>(std::ceil(motion * angles[which] / 720.0F)) + 2U;
        f32 worst = 0.0F;
        u32 checked = 0;
        for (u32 y = row > 30U ? row - 30U : 0U; y < std::min(row + 30U, kHeight); ++y) {
            // Each row's own extent of the box: its top face is not its middle row's width.
            const Span2 covered = box_columns(run, y);
            for (u32 x = 0; x < kWidth; ++x) {
                if (x + clear >= covered.first && x < covered.last + clear) {
                    continue;
                }
                const usize at = (static_cast<usize>(y) * kWidth) + x;
                const f32 before = luminance_of(run.input()[at]);
                const f32 after = luminance_of(run.output()[at]);
                worst = std::max(worst, std::fabs(after - before) / std::max(before, 1e-3F));
                ++checked;
            }
        }
        std::fprintf(stderr, "(c) background beyond the streak: %u px, worst relative change %g\n",
                     checked, static_cast<double>(worst));
        CY_CHECK_GT(checked, 10000U);
        CY_CHECK_LT(worst, 2e-3F);

        // THE DEVICE IS THE REFERENCE: the host's filter over the very inputs the gather read.
        MotionBlurInputs inputs;
        inputs.width = kWidth;
        inputs.height = kHeight;
        inputs.color = Span<const Vec4>(run.input().data(), kPixels);
        inputs.velocity = Span<const Vec2>(run.velocity().data(), kPixels);
        inputs.depth = Span<const f32>(run.depth().data(), kPixels);
        std::vector<Vec4> reference(kPixels);
        CY_REQUIRE(motion_blur_reference(inputs, run.pass().constants(),
                                         Span<Vec4>(reference.data(), reference.size()))
                       .has_value());
        u32 disagreeing = 0;
        for (usize at = 0; at < kPixels; ++at) {
            const f32 device = luminance_of(run.output()[at]);
            const f32 host = luminance_of(reference[at]);
            disagreeing += static_cast<u32>(std::fabs(device - host) >
                                            (1e-2F * std::max(host, 1e-3F)) + 1e-4F);
        }
        std::fprintf(stderr, "(c) device against the host reference: %u of %zu pixels differ\n",
                     disagreeing, kPixels);
        CY_CHECK_LT(disagreeing, static_cast<u32>(kPixels / 1000U));
        char name[64];
        (void)std::snprintf(name, sizeof(name), "motion-blur-%.0f.png",
                            static_cast<double>(angles[which]));
        save(name, run.pixels());
    }
    // A whole frame of motion reaches twice as far as half of one. Measured 2.2, in whole texels.
    const f32 ratio = lengths[1] / lengths[0];
    std::fprintf(stderr, "(c) 360 against 180 degrees: %.2f\n", static_cast<double>(ratio));
    CY_CHECK_GE(ratio, 1.7F);
    CY_CHECK_LE(ratio, 2.6F);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(d) a closed shutter is the frame without the stage, byte for byte") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    RunOptions closed;
    closed.motion_blur = true;
    closed.settings = shutter(0.0F);
    MotionRun with_stage(fixture, closed);
    RunOptions absent;
    MotionRun without_stage(fixture, absent);
    for (u32 index = 0; index < kFrames; ++index) {
        const Vec3 centre{kBoxStart.x + (kStep * static_cast<f32>(index)), kBoxStart.y,
                          kBoxStart.z};
        CY_REQUIRE(with_stage.frame(centre));
        CY_REQUIRE(without_stage.frame(centre));
        CY_CHECK_EQ(differing(with_stage.pixels(), without_stage.pixels()), usize{0});
    }
    CY_CHECK_GT(with_stage.report().passes_declared, without_stage.report().passes_declared);
    save("motion-blur-closed-shutter.png", with_stage.pixels());

    // The control: an open shutter is a different frame. ALSO THE REGRESSION CASE for
    // `frame_recorder.cpp`'s `bind_frame_sets`, which bound the post-process's descriptor sets
    // before its pipeline: after the gather's compute dispatch they went to the compute bind point,
    // the resolve sampled the temporal pass's set, and this frame came out identical to the one
    // without the stage — so did the closed-shutter check above, vacuously.
    RunOptions open = closed;
    open.settings = shutter(180.0F);
    MotionRun blurred(fixture, open);
    CY_REQUIRE(blurred.slide());
    CY_CHECK_GT(differing(blurred.pixels(), without_stage.pixels()), usize{500});
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(f) a mesh deformed on the device moves by its stored previous vertices, unasked") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // `temporal-rendering`'s "skinned motion is correct without extra work": the previous pose is
    // ALREADY STORED — the other half of the skin's double-buffered output — and the draw names
    // it. The box's placement does not move; only its vertices do, by 0.3 m along +x (the cube is
    // a metre across, so model and world metres agree). Both parities: last frame's half below
    // this frame's in the stream, and above it.
    const Vec3 offset{0.3F, 0.0F, 0.0F};
    for (const bool current_second : {true, false}) {
        RunOptions options;
        options.motion_blur = true;
        options.settings = shutter(0.0F);
        MotionRun run(fixture, options);
        CY_REQUIRE(run.scene().deform_box(1, offset, current_second).has_value());
        CY_REQUIRE(run.frame(kBoxStart));
        CY_REQUIRE(run.frame(kBoxStart));
        const VelocityCheck check = check_velocity(run, kBoxStart - offset, Vec3{});
        std::fprintf(stderr,
                     "(f) this frame's vertices in the %s half: box %u px worst %.4f px (object "
                     "share %.2f px), still worst %.4f px\n",
                     current_second ? "upper" : "lower", check.box_pixels,
                     static_cast<double>(check.worst_box),
                     static_cast<double>(check.box_object_share),
                     static_cast<double>(check.worst_still));
        CY_CHECK_GT(check.box_pixels, 1500U);
        CY_CHECK_LT(check.contact_pixels, 16U);
        CY_CHECK_LT(check.worst_box, kVelocityTolerancePixels);
        CY_CHECK_LT(check.worst_still, kVelocityTolerancePixels);
        CY_CHECK_GT(check.box_object_share, 10.0F);
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
