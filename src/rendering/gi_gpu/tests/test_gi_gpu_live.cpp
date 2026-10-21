// SPDX-License-Identifier: MIT
// The device GI as one frame feature, and its debug views drawn by the device. Issue #35, the live
// half: what the editor's viewport and a game's host attach.
//
// ================================================================================================
// WHAT THIS SUITE ASSERTS
// ================================================================================================
//
//   (a) `GpuGiFrame` attached to the pipeline suites' frame lights it, and under a mode that admits
//       no dynamic bounce in it draws the frame without it, byte for byte.
//   (b) What a host changes in the system between two frames is in the device frame: an emitter
//       rule that puts a lamp out darkens what the lamp lit, and the mode switches the GI off.
//   (c) Every one of the nineteen views the device draws is the host's (`render_gi_debug_view`)
//       over the frame's own surfaces, word for word up to each view's stated bound; the views
//       whose datum is the device frame's own are held to that datum (the published targets, the
//       reprojection's history, the cache query, the lookup's page).
//   (d) The cost: the GI's device time and the time of a whole frame with each view drawn.
//
// It writes the frame lit and in four views (`gi_live_*.png`) into the test's binary directory.

#include "frame_gi_support.h"
#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/debug_views.h>
#include <cy/rendering/gi/emissive.h>
#include <cy/rendering/gi_gpu/gpu_frame.h>
#include <cy/rendering/lightmaps/gi_debug_overlay.h>
#include <cy/servers/render/types.h>
#include <cy/test/test.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace cy;
namespace gi = cy::rendering::gi;
namespace gi_gpu = cy::rendering::gi_gpu;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
using gi_support::GiOfScene;

namespace {

constexpr u32 kDiffuseSlot = 117;
constexpr u32 kSpecularSlot = 118;
constexpr u32 kViewSlot = 119;
/// The face of the scene's third box that faces the camera: the lamp (b) puts out.
constexpr u32 kLampFace = (3U * 6U) + 5U;

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
        description.application_name = "cy_test_render_gi_gpu_live";
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
               device_.value()->capabilities().backend() == rhi::BackendKind::Vulkan &&
               gi_gpu::GpuGiFrame::supported(*device_.value());
    }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] u32 validation_errors() const noexcept { return errors_; }
    void report_skip() const noexcept {
        std::fprintf(
            stderr, "no requested graphics device on this machine; the backend selected was '%s'\n",
            selection_.selected != nullptr ? selection_.selected : "(none)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

/// The pipeline scene lit through `GpuGiFrame`, the way a host attaches it.
class LiveRun {
public:
    struct Options {
        bool attached = true;
        gi::GiMode mode = gi::GiMode::Dynamic;
        bool lamp = false;
        bool readback = false;
        /// Ray queries where the device has them; off traces the field, the views' oracle's tier.
        bool hardware = false;
        /// The caches shaded and gathered on the device, the default; off keeps them the host's,
        /// so a view drawn this frame reads exactly the state the host's view reads.
        bool device_caches = true;
    };

    LiveRun(DeviceFixture& fixture, const Options& options)
        : options_(options), scene_(allocator()) {
        FrameSceneHooks hooks;
        hooks.user = this;
        hooks.configure = &configure;
        hooks.before_assemble = &before_assemble;
        hooks.before_upload = &before_upload;
        scene_.set_hooks(hooks);
        if (const Status built = scene_.build(fixture.device()); !built) {
            std::fprintf(stderr, "gi live: build failed: %s\n", built.error().message);
            return;
        }
        scene_.set_read_back(true);
        if (!options.attached) {
            ready_ = true;
            return;
        }
        if (!gi_.build(scene_.boxes(), options.lamp ? kLampFace : ~0U,
                       Vec3{60000.0F, 52000.0F, 30000.0F})) {
            return;
        }
        gi::GiRuntimeSettings settings = gi_.system.runtime_settings();
        settings.mode = options.mode;
        settings.hardware_ray_tracing = options.hardware;
        if (!gi_.system.set_runtime_settings(settings).has_value()) {
            return;
        }
        gi_gpu::GpuGiFrameDescription description;
        description.width = kWidth;
        description.height = kHeight;
        description.hardware = options.hardware;
        description.readback = options.readback;
        description.device_caches = options.device_caches;
        description.scene.max_window_bricks = 16;
        description.scene.max_brick_slots = 16384;
        description.scene.max_cards = 8192;
        description.scene.max_selection = 2048;
        const Status made = frame_.create(allocator(), fixture.device(), gi_.system, description);
        if (!made) {
            std::fprintf(stderr, "gi live: create failed: %s\n", made.error().message);
            return;
        }
        ready_ = true;
    }

    [[nodiscard]] bool render(u32 frames) {
        if (!ready_) {
            return false;
        }
        for (u32 index = 0; index < frames; ++index) {
            const auto started = std::chrono::steady_clock::now();
            const Status rendered = scene_.render(RecordMode::Callbacks, report_);
            last_frame_ms_ =
                std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started)
                    .count();
            if (!rendered) {
                std::fprintf(stderr, "gi live: render failed: %s\n", rendered.error().message);
                return false;
            }
        }
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        return true;
    }

    [[nodiscard]] bool view(gi::GiDebugView which) {
        gi_gpu::GpuGiDebugViewSettings settings;
        settings.view = which;
        settings.field_slice_height = 0.5F;
        view_mode_ = which == gi::GiDebugView::Count
                         ? render::DebugViewMode::Off
                         : render::debug_view_mode_from_name(gi::gi_debug_view_name(which));
        return frame_.set_debug_view(settings).has_value();
    }

    [[nodiscard]] const std::vector<u32>& pixels() const { return pixels_; }
    [[nodiscard]] gi::IlluminationSystem& system() { return gi_.system; }
    [[nodiscard]] gi_gpu::GpuGiFrame& frame() { return frame_; }
    [[nodiscard]] FrameScene& scene() { return scene_; }
    [[nodiscard]] u64 frames() const { return frame_index_; }
    [[nodiscard]] f64 last_frame_ms() const { return last_frame_ms_; }
    [[nodiscard]] u32 lamp_material() const { return gi_.emissive_material; }

private:
    static void configure(rendering::assembly::AssemblyDescription& description,
                          void* user) noexcept {
        const auto* run = static_cast<const LiveRun*>(user);
        description.pin_jitter = true;
        description.temporal.jitter.spread = 0.0F;
        description.global_illumination = run->options_.attached;
    }

    static Status before_assemble(rendering::RenderGraph& graph,
                                  rendering::assembly::AssemblyView& view,
                                  rendering::assembly::FrameSinks& sinks, void* user) noexcept {
        auto* run = static_cast<LiveRun*>(user);
        if (!run->options_.attached) {
            return ok();
        }
        // THE HOST'S UPDATE, as a host runs it: its lights, its camera, its frame.
        gi::FrameContext context;
        context.camera = Vec3{0.0F, 0.0F, 0.0F};
        context.lights = {run->gi_.lights.data(), run->gi_.lights.size()};
        context.frame = run->frame_index_;
        (void)run->gi_.system.update(context);
        gi_gpu::GpuGiFrameView frame_view;
        frame_view.eye = Vec3{0.0F, 0.0F, 0.0F};
        frame_view.world_to_clip = run->scene_.projection() * run->scene_.view();
        frame_view.projection = run->scene_.projection();
        frame_view.relative_to_view = run->scene_.view();
        gi_gpu::GpuGiFrameTargets targets;
        if (Status prepared = run->frame_.prepare(graph, frame_view, run->frame_index_, targets);
            !prepared) {
            return prepared;
        }
        view.screen_space_gi = targets.diffuse;
        view.screen_space_gi_specular = targets.specular;
        sinks.screen_space_gi = run->frame_.stage();
        run->view_read_ =
            rendering::FrameResourceRead{targets.debug_view, rhi::Access::FragmentSampledRead};
        if (targets.debug_view != rendering::kInvalidResource) {
            sinks.passes[static_cast<usize>(rendering::FramePassKind::Opaque)].reads =
                Span<const rendering::FrameResourceRead>(&run->view_read_, 1);
        }
        run->frame_index_ += 1;
        return ok();
    }

    static Status before_upload(rendering::pipeline::FrameUpload& upload, void* user) noexcept {
        auto* run = static_cast<LiveRun*>(user);
        if (!run->options_.attached) {
            return ok();
        }
        run->frame_.set_jitter(
            Vec2{upload.view.temporal_jitter[0], upload.view.temporal_jitter[1]});
        rendering::pipeline::MaterialTextureSlot slots[3] = {
            {kDiffuseSlot, run->frame_.diffuse_view()},
            {kSpecularSlot, run->frame_.specular_view()},
            {kViewSlot, run->frame_.debug_view()},
        };
        const bool drawn = run->frame_.views().active();
        if (Status set = run->scene_.set_frame_textures(
                Span<const rendering::pipeline::MaterialTextureSlot>(slots, drawn ? 3U : 2U));
            !set) {
            return set;
        }
        if (run->frame_.composites()) {
            run->frame_.write_control(kDiffuseSlot, kSpecularSlot, upload.view.gi_control);
        }
        if (drawn) {
            (void)rendering::lightmaps::write_gi_debug_view(run->view_mode_, kViewSlot,
                                                            upload.view);
        }
        return ok();
    }

    Options options_;
    FrameScene scene_;
    GiOfScene gi_;
    gi_gpu::GpuGiFrame frame_;
    rendering::assembly::AssemblyReport report_{};
    rendering::FrameResourceRead view_read_{};
    render::DebugViewMode view_mode_ = render::DebugViewMode::Off;
    std::vector<u32> pixels_;
    u64 frame_index_ = 0;
    f64 last_frame_ms_ = 0.0;
    bool ready_ = false;
};

[[nodiscard]] usize differing(const std::vector<u32>& a, const std::vector<u32>& b) noexcept {
    usize count = 0;
    for (usize index = 0; index < a.size() && index < b.size(); ++index) {
        count += static_cast<usize>(a[index] != b[index]);
    }
    return count + (a.size() > b.size() ? a.size() - b.size() : b.size() - a.size());
}

void write_picture(const std::vector<u32>& pixels, const char* name) {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, {pixels.data(), pixels.size()}, kWidth, kHeight).has_value()) {
        return;
    }
    const std::string path = std::string(CY_TEST_BINARY_DIR) + "/" + name;
    if (render_test::write_png(path.c_str(), image).has_value()) {
        std::fprintf(stderr, "wrote %s\n", path.c_str());
    }
}

/// What the device frame read back: its surfaces, history and published targets, a frame pixel
/// each (the grid is the frame's at divisor one).
struct FrameReadback {
    std::vector<Vec3> position;
    std::vector<f32> depth;
    std::vector<Vec3> normal;
    std::vector<f32> roughness;
    std::vector<i32> source;
    std::vector<f32> confidence;
    std::vector<Vec3> diffuse;
    std::vector<Vec3> specular;

    [[nodiscard]] bool read(const gi_gpu::GpuScreenResolve& resolve) {
        const usize pixels = static_cast<usize>(kWidth) * kHeight;
        position.resize(pixels);
        depth.resize(pixels);
        normal.resize(pixels);
        roughness.resize(pixels);
        source.resize(pixels);
        confidence.resize(pixels);
        diffuse.resize(pixels);
        specular.resize(pixels);
        const bool read =
            resolve.divisor() == 1U &&
            resolve
                .read_back_surfaces({position.data(), pixels}, {depth.data(), pixels},
                                    {normal.data(), pixels}, {roughness.data(), pixels})
                .has_value() &&
            resolve.read_back_history({source.data(), pixels}, {confidence.data(), pixels})
                .has_value() &&
            resolve.read_back_published({diffuse.data(), pixels}, {specular.data(), pixels})
                .has_value();
        return read;
    }
};

/// The largest difference between two packed words' channels, in 8-bit steps.
[[nodiscard]] u32 apart(u32 a, u32 b) noexcept {
    u32 worst = 0;
    for (u32 shift = 0; shift < 32U; shift += 8U) {
        const auto x = static_cast<i32>((a >> shift) & 0xFFU);
        const auto y = static_cast<i32>((b >> shift) & 0xFFU);
        worst = std::max(worst, static_cast<u32>(x > y ? x - y : y - x));
    }
    return worst;
}

/// The page the device's lookup (`CardSnapshot::find`, the surface cache's own rule) resolves a
/// surface to, by brute force over every page.
[[nodiscard]] u32 lookup_page(const gi::SurfaceCache& surfaces, Vec3 position, Vec3 normal) {
    const f32 radius = surfaces.lookup_radius();
    u32 best = ~0U;
    f32 best_score = 1.0e9F;
    f32 best_alignment = 0.0F;
    const Span<const gi::SurfacePage> pages = surfaces.pages();
    for (u32 handle = 0; handle < pages.size(); ++handle) {
        const gi::SurfacePage& page = pages[handle];
        if (!page.live || !page.valid) {
            continue;
        }
        const f32 alignment = dot(page.normal, normal);
        if (alignment <= gi::kCardLookupAlignment) {
            continue;
        }
        const f32 squared = distance_squared(page.position, position);
        if (squared > radius * radius) {
            continue;
        }
        const f32 score = squared / alignment;
        if (gi::card_lookup_prefers(score, alignment, handle, best_score, best_alignment, best)) {
            best_score = score;
            best_alignment = alignment;
            best = handle;
        }
    }
    return best;
}

/// The host's words for `view` over the device frame's surfaces at `exposure`.
[[nodiscard]] std::vector<u32> host_view(gi::IlluminationSystem& system, gi::GiDebugView view,
                                         const FrameReadback& frame, const Mat4& world_to_clip,
                                         u64 frame_index, f32 exposure) {
    const usize pixels = static_cast<usize>(kWidth) * kHeight;
    std::vector<u32> out(pixels, 0U);
    std::vector<gi::GiViewPixel> surface(pixels);
    for (usize index = 0; index < pixels; ++index) {
        surface[index].position = frame.position[index];
        surface[index].normal = frame.normal[index];
        surface[index].depth = frame.depth[index];
        surface[index].roughness = frame.roughness[index];
        // The device knows a surface's emission as the card its lookup resolves.
        if (frame.depth[index] > 0.0F) {
            const u32 page =
                lookup_page(system.surfaces(), frame.position[index], frame.normal[index]);
            if (page != ~0U) {
                surface[index].emission = system.surfaces().pages()[page].emission;
            }
        }
    }
    const auto pack = [&](usize index, Vec3 colour) {
        out[index] = frame.depth[index] > 0.0F ? gi::pack_colour(colour) : 0U;
    };
    switch (view) {
        case gi::GiDebugView::IndirectDiffuse:
        case gi::GiDebugView::IndirectSpecular:
            for (usize index = 0; index < pixels; ++index) {
                const Vec3 radiance = view == gi::GiDebugView::IndirectDiffuse
                                          ? frame.diffuse[index]
                                          : frame.specular[index];
                out[index] = frame.depth[index] > 0.0F ? gi::pack_radiance(radiance, exposure) : 0U;
            }
            return out;
        case gi::GiDebugView::Confidence:
            for (usize index = 0; index < pixels; ++index) {
                const gi::RadianceSample cached = system.radiance().sample(
                    frame.position[index], frame.normal[index], frame_index);
                pack(index, gi::debug_view_ramp(1.0F - cached.confidence));
            }
            return out;
        case gi::GiDebugView::Cards:
            for (usize index = 0; index < pixels; ++index) {
                const u32 page =
                    frame.depth[index] > 0.0F
                        ? lookup_page(system.surfaces(), frame.position[index], frame.normal[index])
                        : ~0U;
                pack(index, page == ~0U ? Vec3{0.08F, 0.08F, 0.08F} : gi::debug_view_hue(page));
            }
            return out;
        case gi::GiDebugView::RayCount:
            // The world tier the reflection's rays take, `reflection_ray_count` of them.
            for (usize index = 0; index < pixels; ++index) {
                const f32 rays =
                    static_cast<f32>(gi::reflection_ray_count(
                        frame.roughness[index], system.settings().reflection_thresholds)) /
                    16.0F;
                pack(index, Vec3{0.0F, rays, 0.0F});
            }
            return out;
        default:
            break;
    }
    gi::GiViewCamera camera;
    camera.width = kWidth;
    camera.height = kHeight;
    camera.position = Vec3{0.0F, 0.0F, 0.0F};
    camera.world_to_clip = world_to_clip;
    gi::GiDebugInputs inputs;
    inputs.pixels = {surface.data(), surface.size()};
    inputs.history.source = {frame.source.data(), frame.source.size()};
    inputs.history.confidence = {frame.confidence.data(), frame.confidence.size()};
    inputs.field_slice_height = 0.5F;
    inputs.frame = frame_index;
    inputs.exposure = exposure;
    (void)gi::render_gi_debug_view(system, view, camera, inputs, {out.data(), out.size()});
    return out;
}

}  // namespace

CY_TEST_CASE("(a) attached through GpuGiFrame the GI lights the frame, and off it is the frame") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    LiveRun off(fixture, {.attached = false});
    CY_REQUIRE(off.render(8));
    LiveRun on(fixture, {});
    CY_REQUIRE(on.render(8));
    LiveRun none(fixture, {.mode = gi::GiMode::None});
    CY_REQUIRE(none.render(8));
    LiveRun baked(fixture, {.mode = gi::GiMode::Baked});
    CY_REQUIRE(baked.render(8));
    const usize changed = differing(off.pixels(), on.pixels());
    std::fprintf(stderr,
                 "GpuGiFrame on changes %zu of %u pixels; mode None changes %zu, Baked %zu\n",
                 changed, kWidth * kHeight, differing(off.pixels(), none.pixels()),
                 differing(off.pixels(), baked.pixels()));
    CY_CHECK(on.frame().composites());
    CY_CHECK_FALSE(none.frame().composites());
    CY_CHECK_GT(changed, static_cast<usize>(kWidth * kHeight / 10U));
    CY_CHECK_EQ(differing(off.pixels(), none.pixels()), 0U);
    CY_CHECK_EQ(differing(off.pixels(), baked.pixels()), 0U);
    write_picture(off.pixels(), "gi_live_off.png");
    write_picture(on.pixels(), "gi_live_on.png");
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(b) the host's emitter rule and mode, changed between frames, are the device's") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // The same lit frame twice; in the second the lamp is put out at frame 6, as a host's script
    // or the editor's `gi.emission.set` does it — through `set_emitter_rules`, nothing rebuilt.
    LiveRun lit(fixture, {.lamp = true, .readback = true});
    CY_REQUIRE(lit.render(16));
    LiveRun dark(fixture, {.lamp = true, .readback = true});
    CY_REQUIRE(dark.render(6));
    CY_REQUIRE(dark.lamp_material() != 0U);
    const gi::EmitterRule out{gi::kAnyEmitter, dark.lamp_material(), gi::EmitterOverride::Off};
    CY_REQUIRE(dark.system().set_emitter_rules({&out, 1}, dark.frames()).has_value());
    CY_REQUIRE(dark.render(10));
    const usize pixels = static_cast<usize>(kWidth) * kHeight;
    std::vector<Vec3> lit_diffuse(pixels);
    std::vector<Vec3> dark_diffuse(pixels);
    std::vector<Vec3> unused(pixels);
    CY_REQUIRE(lit.frame()
                   .resolve()
                   .read_back_published({lit_diffuse.data(), pixels}, {unused.data(), pixels})
                   .has_value());
    CY_REQUIRE(dark.frame()
                   .resolve()
                   .read_back_published({dark_diffuse.data(), pixels}, {unused.data(), pixels})
                   .has_value());
    f64 lit_sum = 0.0;
    f64 dark_sum = 0.0;
    u32 darker = 0;
    for (usize index = 0; index < pixels; ++index) {
        const f32 a = gi::emission_luminance(lit_diffuse[index]);
        const f32 b = gi::emission_luminance(dark_diffuse[index]);
        lit_sum += static_cast<f64>(a);
        dark_sum += static_cast<f64>(b);
        darker += b < a * 0.95F ? 1U : 0U;
    }
    std::fprintf(stderr,
                 "the lamp put out at frame 6: the published indirect diffuse sums %.4g lit and "
                 "%.4g dark; %u of %zu pixels are more than 5%% darker; the frame differs in %zu\n",
                 lit_sum, dark_sum, darker, pixels, differing(lit.pixels(), dark.pixels()));
    CY_CHECK_LT(dark_sum, lit_sum * 0.97);
    CY_CHECK_GT(darker, static_cast<u32>(pixels / 50U));
    CY_CHECK_GT(differing(lit.pixels(), dark.pixels()), static_cast<usize>(pixels / 50U));

    // The mode, from the same host call: None at frame 16 is the frame without the GI.
    LiveRun off(fixture, {.attached = false});
    CY_REQUIRE(off.render(17));
    gi::GiRuntimeSettings settings = dark.system().runtime_settings();
    settings.mode = gi::GiMode::None;
    CY_REQUIRE(dark.system().set_runtime_settings(settings).has_value());
    CY_REQUIRE(dark.render(1));
    CY_CHECK_FALSE(dark.frame().composites());
    // Not byte for byte: the temporal resolve still holds frames lit by the GI. Its share fades;
    // what this checks is that the frame stopped compositing the GI's targets.
    std::fprintf(stderr, "mode None at frame 16: %zu pixels differ from the frame without GI\n",
                 differing(off.pixels(), dark.pixels()));
    CY_CHECK_LT(differing(off.pixels(), dark.pixels()), differing(off.pixels(), lit.pixels()));
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) every view the device draws is the host's over the frame's own surfaces") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // The caches the host's: a device gather would update this frame's probes on the device before
    // the host's copy, and the host's view reads the host's.
    LiveRun run(fixture, {.lamp = true, .readback = true, .device_caches = false});
    CY_REQUIRE(run.render(6));
    const Mat4 world_to_clip = run.scene().projection() * run.scene().view();
    const usize pixels = static_cast<usize>(kWidth) * kHeight;
    // THE PUBLISHED TARGETS ARE HALVES. The host copy `read_back_published` gives is the float the
    // publish computed; the texture the forward pass samples — and the view reads — holds it
    // narrowed by the store, which Vulkan lets round to nearest or toward zero (the RTX 5060
    // truncates). So those two views are held to one 8-bit step a channel, and the exposure, the
    // mean of the halves, to two of a half's ulps (1e-3) of the floats' mean.
    //
    // Each view's bound: the share of the frame's words that may differ from the host's. Zero
    // where the device reads the same datum the host function computes in the same order; the
    // disc views' rims and centres are within a last-bit of a pixel's edge (`std::hypot` and
    // `rsqrt` against `sqrt`), and the leak view's suspects come from every contributing probe
    // where the host's report keeps the first 32.
    struct Bound {
        gi::GiDebugView view = gi::GiDebugView::Count;
        f64 share = 0.0;
        /// 8-bit steps a channel may differ by before the word counts as different.
        u32 steps = 0;
    };
    const Bound bounds[] = {
        {gi::GiDebugView::Probes, 0.002},
        {gi::GiDebugView::ProbeIrradiance, 0.002},
        {gi::GiDebugView::ProbeAge, 0.002},
        {gi::GiDebugView::ProbeError, 0.002},
        {gi::GiDebugView::ProbeVisibility, 0.01},
        {gi::GiDebugView::RadianceClipmap, 0.001},
        {gi::GiDebugView::SurfaceCache, 0.001},
        {gi::GiDebugView::DistanceField, 0.002},
        {gi::GiDebugView::Cards, 0.001},
        {gi::GiDebugView::ReflectionProbes, 0.0},
        {gi::GiDebugView::ScreenTrace, 0.0},
        {gi::GiDebugView::Confidence, 0.001},
        {gi::GiDebugView::Tier, 0.002},
        {gi::GiDebugView::RayCount, 0.0},
        {gi::GiDebugView::DenoiserConfidence, 0.0},
        {gi::GiDebugView::Disocclusion, 0.0},
        {gi::GiDebugView::IndirectDiffuse, 0.0, 1},
        {gi::GiDebugView::IndirectSpecular, 0.0, 1},
        {gi::GiDebugView::Emissive, 0.002},
    };
    static_assert(sizeof(bounds) / sizeof(bounds[0]) == gi::kGiDebugViewCount);
    std::vector<u32> device_words(pixels);
    for (const Bound& bound : bounds) {
        CY_REQUIRE(run.view(bound.view));
        CY_REQUIRE(run.render(1));
        CY_REQUIRE(run.frame().views().read_back({device_words.data(), pixels}).has_value());
        FrameReadback frame;
        CY_REQUIRE(frame.read(run.frame().resolve()));
        const Expected<f32, Error> exposure = run.frame().views().read_back_exposure();
        CY_REQUIRE(exposure.has_value());
        const std::vector<u32> host =
            host_view(run.system(), bound.view, frame, world_to_clip, run.frames() - 1U, *exposure);
        usize drawn = 0;
        usize mismatched = 0;
        for (usize index = 0; index < pixels; ++index) {
            drawn += (device_words[index] >> 24U) != 0U || (host[index] >> 24U) != 0U ? 1U : 0U;
            mismatched += apart(device_words[index], host[index]) > bound.steps ? 1U : 0U;
        }
        const f64 share = static_cast<f64>(mismatched) / static_cast<f64>(pixels);
        std::fprintf(stderr,
                     "%-22s %6zu texels drawn, %4zu differ from the host's by more than %u "
                     "steps (%.4f, bound %.4f)\n",
                     gi::gi_debug_view_name(bound.view), drawn, mismatched, bound.steps, share,
                     bound.share);
        CY_CHECK_GT(drawn, static_cast<usize>(0));
        CY_CHECK_LE(share, bound.share);
        if (bound.view == gi::GiDebugView::IndirectDiffuse) {
            // The automatic exposure: the frame's own mean indirect diffuse at a third of white.
            f64 sum = 0.0;
            u32 count = 0;
            for (usize index = 0; index < pixels; index += 8U) {
                if (frame.depth[index] > 0.0F) {
                    sum += static_cast<f64>(gi::emission_luminance(frame.diffuse[index]));
                    count += 1;
                }
            }
            const f64 expected = 0.5 / (sum / static_cast<f64>(count));
            std::fprintf(stderr, "automatic exposure %.6g, the host's mean says %.6g\n",
                         static_cast<f64>(*exposure), expected);
            CY_CHECK_LT(std::fabs(static_cast<f64>(*exposure) - expected), expected * 1.0e-3);
        }
        if (bound.view == gi::GiDebugView::Tier || bound.view == gi::GiDebugView::Probes ||
            bound.view == gi::GiDebugView::IndirectDiffuse ||
            bound.view == gi::GiDebugView::Cards) {
            const std::string name =
                std::string("gi_live_") + gi::gi_debug_view_name(bound.view) + ".png";
            write_picture(run.pixels(), name.c_str());
        }
    }
    // The view drawn over the frame: where it is opaque the frame shows it, where it is clear the
    // lit frame does — the forward pass's own overlay rule, through the device texture.
    CY_REQUIRE(run.view(gi::GiDebugView::Count));
    CY_REQUIRE(run.render(1));
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(d) the cost: the GI's device time and a whole frame with each view drawn") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    LiveRun run(fixture, {.lamp = true});
    CY_REQUIRE(run.render(10));
    const gi_gpu::GpuGiCost cost = run.frame().last_cost();
    std::fprintf(stderr,
                 "the GI at %ux%u on the device: the resolve %.3f ms, the card shading %.3f ms, "
                 "the probe gather %.3f ms; a whole frame %.2f ms\n",
                 kWidth, kHeight, cost.resolve_ms, cost.cards_ms, cost.gather_ms,
                 run.last_frame_ms());
    CY_CHECK(cost.resolve_valid);
    CY_CHECK_LT(cost.total_ms(), 8.0);
    f64 worst = 0.0;
    for (u32 which = 0; which < gi::kGiDebugViewCount; ++which) {
        const auto view = static_cast<gi::GiDebugView>(which);
        CY_REQUIRE(run.view(view));
        CY_REQUIRE(run.render(3));
        // The best of three frames: the first after a switch also creates its descriptor sets.
        f64 best = run.last_frame_ms();
        for (u32 again = 0; again < 2; ++again) {
            CY_REQUIRE(run.render(1));
            best = std::min(best, run.last_frame_ms());
        }
        worst = std::max(worst, best);
        std::fprintf(stderr, "%-22s a whole frame %.2f ms\n", gi::gi_debug_view_name(view), best);
    }
    // INTERACTIVE: every view is a frame of at least 20 a second, the scene, the GI, the host's
    // cache update and the readback of the frame included, where the host's views took 35 to
    // 240 ms at 320x180 for the view alone.
    CY_CHECK_LT(worst, 50.0);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
