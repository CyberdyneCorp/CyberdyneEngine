// SPDX-License-Identifier: MIT
// The per-pixel GI in the forward frame: the pipeline suites' scene with the frame's
// `ScreenSpaceGi` stage produced by `GpuScreenResolve` and composited by `cy/frame.slang` where the
// irradiance volume's ambient sits. Issue #35, stage 4.
//
// ================================================================================================
// WHAT THIS SUITE ASSERTS
// ================================================================================================
//
//   (a) the stage attached and its words left at "none" draws the frame without it, byte for byte:
//       the composite is inert until a caller binds it. (The frame without the stage is held to
//       the committed references rendered before this change by the pipeline, ambient occlusion,
//       fog and lightmap suites, which run the same shader.)
//   (b) bound, the GI changes the picture; bound under a mode `exclusion_for()` admits no dynamic
//       bounce in, it draws the frame without it, byte for byte — the double-counting rule at the
//       composite.
//   (c) the prepass unpacks into the surfaces the resolve reads: every pixel that sees a box lands
//       on that box, at the depth the ray through its centre meets it, facing the way it faces.
//
// It writes the scene with the GI off and on beside each other (`gi_frame_off.png`,
// `gi_frame_on.png`) into the test's binary directory.

#include "frame_gi_support.h"
#include "frame_scene.h"
#include "golden.h"
#include "screen_support.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/emissive.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/gi_gpu/frame_lights.h>
#include <cy/rendering/gi_gpu/gpu_radiance_cache.h>
#include <cy/rendering/gi_gpu/gpu_resolve.h>
#include <cy/rendering/gi_gpu/gpu_scene.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/rendering/lighting/units.h>
#include <cy/test/test.h>

#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace cy;
namespace gi = cy::rendering::gi;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
using cy::rendering::gi_gpu::GpuGiScene;
using cy::rendering::gi_gpu::GpuGiSceneDescription;
using cy::rendering::gi_gpu::GpuRadianceCache;
using cy::rendering::gi_gpu::GpuRadianceCacheDescription;
using cy::rendering::gi_gpu::GpuResolveView;
using cy::rendering::gi_gpu::GpuScreenResolve;
using cy::rendering::gi_gpu::GpuScreenResolveDescription;

namespace {

/// The two slots of the frame's set 0 texture table the targets are bound at. Any free slots; the
/// scene binds no material texture of its own here.
constexpr u32 kDiffuseSlot = 118;
constexpr u32 kSpecularSlot = 119;

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
        description.application_name = "cy_test_render_gi_gpu_frame";
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
               GpuScreenResolve::supported(*device_.value());
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

using gi_support::GiOfScene;

/// One scene, its GI, and the hooks that attach the stage.
struct GiFrameCase {
    FrameScene* scene = nullptr;
    GiOfScene* gi = nullptr;
    GpuScreenResolve* resolve = nullptr;
    /// The stage in the frame at all.
    bool attached = false;
    /// The composite's words written.
    bool bound = false;
    gi::GiMode mode = gi::GiMode::Dynamic;
    /// The specular target's slot left at none: the diffuse alone is composited.
    bool diffuse_only = false;
    u64 frame = 0;
    /// When set, the frame's lights are these instead of the scene's sun and two lamps.
    const std::vector<render::LightDescription>* lights = nullptr;
};

void replace_lights(Array<render::LightDescription>& lights, void* user) noexcept {
    const auto* frame = static_cast<const GiFrameCase*>(user);
    if (frame->lights == nullptr) {
        return;
    }
    lights.clear();
    for (const render::LightDescription& light : *frame->lights) {
        (void)lights.push_back(light);
    }
}

void configure_case(rendering::assembly::AssemblyDescription& description, void* user) noexcept {
    const auto* frame = static_cast<const GiFrameCase*>(user);
    description.pin_jitter = true;
    // No jitter: (c) compares each pixel with the ray through its centre.
    description.temporal.jitter.spread = 0.0F;
    description.global_illumination = frame->attached;
}

Status before_assemble(rendering::RenderGraph& graph, rendering::assembly::AssemblyView& view,
                       rendering::assembly::FrameSinks& sinks, void* user) noexcept {
    auto* frame = static_cast<GiFrameCase*>(user);
    if (!frame->attached) {
        return ok();
    }
    gi::FrameContext context;
    context.camera = Vec3{0.0F, 0.0F, 0.0F};
    context.lights = {frame->gi->lights.data(), frame->gi->lights.size()};
    context.frame = frame->frame;
    (void)frame->gi->system.update(context);
    if (Status synced = frame->resolve->sync(frame->gi->system.radiance()); !synced) {
        return synced;
    }
    frame->resolve->set_cache_frame(frame->frame);
    GpuResolveView resolve_view;
    resolve_view.camera = Vec3{0.0F, 0.0F, 0.0F};
    resolve_view.world_to_clip = frame->scene->projection() * frame->scene->view();
    const Mat4& projection = frame->scene->projection();
    resolve_view.projection[0] = projection.columns[0].x;
    resolve_view.projection[1] = projection.columns[1].y;
    resolve_view.projection[2] = projection.columns[2].z;
    resolve_view.projection[3] = projection.columns[3].z;
    resolve_view.relative_to_view = frame->scene->view();
    if (Status began = frame->resolve->begin_frame(resolve_view, frame->frame); !began) {
        return began;
    }
    view.screen_space_gi = frame->resolve->import_diffuse(graph);
    view.screen_space_gi_specular = frame->resolve->import_specular(graph);
    sinks.screen_space_gi = frame->resolve->stage();
    frame->frame += 1;
    return ok();
}

Status before_upload(rendering::pipeline::FrameUpload& upload, void* user) noexcept {
    auto* frame = static_cast<GiFrameCase*>(user);
    if (!frame->attached) {
        return ok();
    }
    frame->resolve->set_jitter(
        Vec2{upload.view.temporal_jitter[0], upload.view.temporal_jitter[1]});
    if (!frame->bound) {
        return ok();
    }
    const rendering::pipeline::MaterialTextureSlot slots[2] = {
        {kDiffuseSlot, frame->resolve->diffuse_view()},
        {kSpecularSlot, frame->resolve->specular_view()},
    };
    if (Status set = frame->scene->set_frame_textures(
            Span<const rendering::pipeline::MaterialTextureSlot>(slots, 2));
        !set) {
        return set;
    }
    rendering::gi_gpu::write_gi_control(
        kDiffuseSlot, frame->diffuse_only ? rendering::pipeline::kNoMaterialTexture : kSpecularSlot,
        frame->mode, upload.view.gi_control);
    return ok();
}

/// A scene rendered `frames` times with a case's hooks; the last frame's pixels kept.
class GiFrameRun {
public:
    GiFrameRun(DeviceFixture& fixture, bool attached, bool bound, gi::GiMode mode,
               bool diffuse_only = false, gi::GiGather gather = gi::GiGather::ScreenProbes,
               const std::vector<render::LightDescription>* lights = nullptr)
        : scene_(allocator()) {
        case_.lights = lights;
        case_.scene = &scene_;
        case_.gi = &gi_;
        case_.resolve = &resolve_;
        case_.attached = attached;
        case_.bound = bound;
        case_.mode = mode;
        case_.diffuse_only = diffuse_only;
        FrameSceneHooks hooks;
        hooks.user = &case_;
        hooks.configure = &configure_case;
        hooks.before_assemble = &before_assemble;
        hooks.before_upload = &before_upload;
        hooks.lights = &replace_lights;
        scene_.set_hooks(hooks);
        const Status built = scene_.build(fixture.device());
        if (!built) {
            std::fprintf(stderr, "gi frame: build failed: %s\n", built.error().message);
            return;
        }
        scene_.set_read_back(true);
        if (!attached) {
            ready_ = true;
            return;
        }
        GpuGiSceneDescription description;
        description.max_levels = 2;
        description.max_window_bricks = 16;
        description.max_brick_slots = 16384;
        description.max_cards = 8192;
        description.max_selection = 2048;
        GpuScreenResolveDescription resolve;
        resolve.width = kWidth;
        resolve.height = kHeight;
        resolve.readback = true;
        ready_ =
            gi_.build(scene_.boxes()) &&
            gscene_.create(allocator(), fixture.device(), description).has_value() &&
            cache_.create(allocator(), fixture.device(), gscene_, GpuRadianceCacheDescription{})
                .has_value();
        if (ready_) {
            cache_.bind_field(gi_.system.field());
            cache_.bind_host_surfaces(&gi_.system.surfaces());
            ready_ = resolve_.create(allocator(), fixture.device(), cache_, resolve).has_value();
        }
        if (ready_) {
            gi::ScreenResolveSettings settings;
            settings.mode = mode;
            settings.gather = gather;
            settings.max_ray_distance_metres = 24.0F;
            resolve_.set_settings(settings);
            resolve_.set_denoiser(gi_.system.denoiser());
            resolve_.set_field(gi_.system.field());
            resolve_.set_sky(gi_.system.sky_term());
        }
    }

    [[nodiscard]] bool render(u32 frames) {
        if (!ready_) {
            return false;
        }
        for (u32 index = 0; index < frames; ++index) {
            const Status rendered = scene_.render(RecordMode::Callbacks, report_);
            if (!rendered) {
                std::fprintf(stderr, "gi frame: render failed: %s\n", rendered.error().message);
                return false;
            }
        }
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        return true;
    }

    [[nodiscard]] const std::vector<u32>& pixels() const { return pixels_; }
    [[nodiscard]] const rendering::assembly::AssemblyReport& report() const { return report_; }
    [[nodiscard]] FrameScene& scene() { return scene_; }
    [[nodiscard]] GpuScreenResolve& resolve() { return resolve_; }

private:
    FrameScene scene_;
    GiOfScene gi_;
    GpuGiScene gscene_;
    GpuRadianceCache cache_;
    GpuScreenResolve resolve_;
    GiFrameCase case_;
    rendering::assembly::AssemblyReport report_{};
    std::vector<u32> pixels_;
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

}  // namespace

CY_TEST_CASE("(a) the stage attached with its words at none draws the frame without it") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    GiFrameRun absent(fixture, false, false, gi::GiMode::Dynamic);
    CY_REQUIRE(absent.render(3));
    GiFrameRun inert(fixture, true, false, gi::GiMode::Dynamic);
    CY_REQUIRE(inert.render(3));
    std::fprintf(stderr, "passes declared: %u without the stage, %u with it\n",
                 absent.report().passes_declared, inert.report().passes_declared);
    CY_CHECK_GT(inert.report().passes_declared, absent.report().passes_declared);
    CY_CHECK_EQ(differing(absent.pixels(), inert.pixels()), 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(b) bound, the GI changes the picture; under a mode that admits none, it does not") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    GiFrameRun off(fixture, false, false, gi::GiMode::Dynamic);
    CY_REQUIRE(off.render(8));
    GiFrameRun on(fixture, true, true, gi::GiMode::Dynamic);
    CY_REQUIRE(on.render(8));
    GiFrameRun excluded(fixture, true, true, gi::GiMode::Baked);
    CY_REQUIRE(excluded.render(8));
    // THE DIFFUSE ALONE, the specular's slot at none: the composite's replacement of the ambient is
    // a change of its own, not one the specular term's addition stands in for.
    GiFrameRun diffuse(fixture, true, true, gi::GiMode::Dynamic, true);
    CY_REQUIRE(diffuse.render(8));
    const usize changed = differing(off.pixels(), on.pixels());
    const usize diffuse_changed = differing(off.pixels(), diffuse.pixels());
    std::fprintf(stderr,
                 "GI on changes %zu of %u pixels, its diffuse alone %zu; under Baked it changes "
                 "%zu\n",
                 changed, kWidth * kHeight, diffuse_changed,
                 differing(off.pixels(), excluded.pixels()));
    CY_CHECK_GT(changed, static_cast<usize>(kWidth * kHeight / 10U));
    CY_CHECK_GT(diffuse_changed, static_cast<usize>(kWidth * kHeight / 10U));
    CY_CHECK_EQ(differing(off.pixels(), excluded.pixels()), 0U);
    write_picture(off.pixels(), "gi_frame_off.png");
    write_picture(on.pixels(), "gi_frame_on.png");
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) the prepass unpacks into the surfaces the resolve reads") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    GiFrameRun run(fixture, true, false, gi::GiMode::Dynamic);
    CY_REQUIRE(run.render(1));
    const usize pixels = static_cast<usize>(kWidth) * kHeight;
    std::vector<Vec3> position(pixels);
    std::vector<f32> depth(pixels);
    std::vector<Vec3> normal(pixels);
    std::vector<f32> roughness(pixels);
    CY_REQUIRE(run.resolve()
                   .read_back_surfaces({position.data(), pixels}, {depth.data(), pixels},
                                       {normal.data(), pixels}, {roughness.data(), pixels})
                   .has_value());
    // The analytic answer: the scene's camera is at the origin looking down -Z with an identity
    // view, so the ray through a pixel centre is the projection's inverse there.
    std::vector<gi_support::AxisQuad> quads;
    for (const Aabb& box : run.scene().boxes()) {
        const Vec3 lo = box.min;
        const Vec3 hi = box.max;
        quads.push_back({0, lo.x, {lo.y, lo.z}, {hi.y, hi.z}, {}, -1.0F});
        quads.push_back({0, hi.x, {lo.y, lo.z}, {hi.y, hi.z}, {}, 1.0F});
        quads.push_back({1, lo.y, {lo.x, lo.z}, {hi.x, hi.z}, {}, -1.0F});
        quads.push_back({1, hi.y, {lo.x, lo.z}, {hi.x, hi.z}, {}, 1.0F});
        quads.push_back({2, lo.z, {lo.x, lo.y}, {hi.x, hi.y}, {}, -1.0F});
        quads.push_back({2, hi.z, {lo.x, lo.y}, {hi.x, hi.y}, {}, 1.0F});
    }
    const Mat4& projection = run.scene().projection();
    u32 surfaces = 0;
    u32 sky_agrees = 0;
    u32 misplaced = 0;
    u32 misturned = 0;
    f32 worst = 0.0F;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const usize index = (static_cast<usize>(y) * kWidth) + x;
            const f32 ndc_x = (((static_cast<f32>(x) + 0.5F) / kWidth) * 2.0F) - 1.0F;
            const f32 ndc_y = 1.0F - (((static_cast<f32>(y) + 0.5F) / kHeight) * 2.0F);
            const Vec3 ray = normalize(
                Vec3{ndc_x / projection.columns[0].x, ndc_y / projection.columns[1].y, -1.0F});
            f32 t = 0.0F;
            Vec3 facing{0.0F, 1.0F, 0.0F};
            const bool hit = gi_support::cast(quads, Vec3{0.0F, 0.0F, 0.0F}, ray, t, facing);
            if (!hit) {
                sky_agrees += depth[index] <= 0.0F ? 1U : 0U;
                continue;
            }
            if (depth[index] <= 0.0F) {
                continue;  // an edge pixel the rasteriser gave to the sky
            }
            surfaces += 1;
            const Vec3 expected = ray * t;
            const f32 error = length(position[index] - expected) / t;
            worst = std::max(worst, error);
            misplaced += error > 0.01F ? 1U : 0U;
            misturned += dot(normal[index], facing) < 0.99F ? 1U : 0U;
        }
    }
    std::fprintf(stderr,
                 "unpacked %u surface pixels: %u farther than 1%% of their depth from the box the "
                 "ray meets (worst %.4f), %u facing another way; %u sky pixels agree\n",
                 surfaces, misplaced, static_cast<double>(worst), misturned, sky_agrees);
    CY_CHECK_GT(surfaces, pixels / 4U);
    // Silhouette pixels: the rasteriser and the analytic ray may disagree on which box a pixel
    // centre on an edge belongs to.
    CY_CHECK_LT(misplaced, surfaces / 50U);
    CY_CHECK_LT(misturned, surfaces / 50U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(d) with either gather attached at none, the frame is the frame without it") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    GiFrameRun absent(fixture, false, false, gi::GiMode::Dynamic);
    CY_REQUIRE(absent.render(3));
    GiFrameRun per_pixel(fixture, true, false, gi::GiMode::Dynamic, false, gi::GiGather::PerPixel);
    CY_REQUIRE(per_pixel.render(3));
    GiFrameRun probes(fixture, true, false, gi::GiMode::Dynamic, false, gi::GiGather::ScreenProbes);
    CY_REQUIRE(probes.render(3));
    CY_CHECK_EQ(differing(absent.pixels(), per_pixel.pixels()), 0U);
    CY_CHECK_EQ(differing(absent.pixels(), probes.pixels()), 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(e) the gather lever: bound, each gather lights the frame, and differently") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    GiFrameRun absent(fixture, false, false, gi::GiMode::Dynamic);
    CY_REQUIRE(absent.render(8));
    GiFrameRun per_pixel(fixture, true, true, gi::GiMode::Dynamic, false, gi::GiGather::PerPixel);
    CY_REQUIRE(per_pixel.render(8));
    GiFrameRun probes(fixture, true, true, gi::GiMode::Dynamic, false, gi::GiGather::ScreenProbes);
    CY_REQUIRE(probes.render(8));
    const usize per_pixel_changed = differing(absent.pixels(), per_pixel.pixels());
    const usize probes_changed = differing(absent.pixels(), probes.pixels());
    const usize between = differing(per_pixel.pixels(), probes.pixels());
    // The probes' frame traced probes: its last frame's records hold live ones (the tiles whose
    // centre is the scene's backdrop hold none).
    const gi::ScreenProbeLayout& layout = probes.resolve().probe_layout();
    std::vector<gi::ScreenProbe> records(layout.records());
    std::vector<u32> slots(layout.cell_count());
    CY_REQUIRE(probes.resolve()
                   .read_back_probes({records.data(), records.size()}, {slots.data(), slots.size()})
                   .has_value());
    u32 traced = 0;
    for (u32 tile = 0; tile < layout.uniform_count(); ++tile) {
        traced += records[tile].depth > 0.0F && records[tile].history > 0.0F ? 1U : 0U;
    }
    std::fprintf(stderr, "the probes' frame traced %u of %u uniform probes\n", traced,
                 layout.uniform_count());
    CY_CHECK_GT(traced, layout.uniform_count() / 4U);
    std::fprintf(stderr,
                 "bound, the per-pixel gather changes %zu of %u pixels, the screen probes %zu; "
                 "the two frames differ in %zu\n",
                 per_pixel_changed, kWidth * kHeight, probes_changed, between);
    CY_CHECK_GT(per_pixel_changed, static_cast<usize>(kWidth * kHeight / 10U));
    CY_CHECK_GT(probes_changed, static_cast<usize>(kWidth * kHeight / 10U));
    CY_CHECK_GT(between, 0U);
    write_picture(probes.pixels(), "gi_frame_probes.png");
    write_picture(per_pixel.pixels(), "gi_frame_per_pixel.png");
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

namespace {

/// A 30 cm lamp facing down two metres over the floor slab's centre: small and bright, the kind of
/// emitter the GI promotes. Its radiance in the frame's units (nits).
[[nodiscard]] gi::GiLight promoted_lamp() noexcept {
    gi::GiLight lamp;
    lamp.position = Vec3{0.0F, 0.2F, -8.0F};
    lamp.emitter_normal = Vec3{0.0F, -1.0F, 0.0F};
    lamp.emitter_area = 0.09F;
    lamp.colour = Vec3{360000.0F, 320000.0F, 260000.0F};
    lamp.intensity = 1.0F;
    lamp.id = 77;
    return lamp;
}

/// `cy/light.slang`'s `evaluateLight` for a punctual record, transcribed: the illuminance a surface
/// at `receiver` facing `normal` receives, in the frame's lux.
[[nodiscard]] f32 frame_illuminance(const rendering::GpuLight& light, Vec3 receiver,
                                    Vec3 normal) noexcept {
    const Vec3 position{light.position_relative_to_camera[0], light.position_relative_to_camera[1],
                        light.position_relative_to_camera[2]};
    const Vec3 to_light = position - receiver;
    const f32 distance_squared = dot(to_light, to_light);
    const Vec3 direction = to_light / std::sqrt(std::max(distance_squared, 1.0e-8F));
    const f32 factor = distance_squared / std::max(light.range * light.range, 1.0e-6F);
    const f32 window = std::clamp(1.0F - (factor * factor), 0.0F, 1.0F);
    f32 attenuation = (window * window) / std::max(distance_squared, 1.0e-4F);
    const Vec3 axis{light.direction[0], light.direction[1], light.direction[2]};
    const f32 cone =
        std::clamp((dot(-direction, axis) * light.spot_scale) + light.spot_bias, 0.0F, 1.0F);
    if (light.kind == rendering::kGpuLightSpot) {
        attenuation *= cone * cone;
    } else if (light.kind == rendering::kGpuLightEmitter) {
        attenuation *= cone;
    }
    return light.intensity * attenuation * std::max(0.0F, dot(normal, direction));
}

/// The frame's record for `light`, built as the frame assembly builds it.
[[nodiscard]] rendering::GpuLight frame_record(const render::LightDescription& light) noexcept {
    rendering::LightBuildParameters parameters;
    parameters.unit = rendering::default_unit_for(light.kind);
    return rendering::build_gpu_light(light, parameters);
}

}  // namespace

CY_TEST_CASE("(f) a promoted lamp as the frame's light is the light the GI shades its cards with") {
    // No device: the frame's light record against the GI's own integration of the same lamp. The
    // GI shades a card `albedo * L * F` (`emitter_form_factor`, exact); the frame shades a pixel
    // `albedo / pi * E`. So `E / pi` must be the GI's `direct_radiance`, wherever the lamp is small
    // against its distance.
    const gi::GiLight lamp = promoted_lamp();
    const render::LightDescription light =
        rendering::gi_gpu::promoted_frame_light(lamp, 1.0e-3F, true);
    CY_CHECK(light.kind == render::LightKind::Spot);
    CY_CHECK(light.cosine_falloff);
    const rendering::GpuLight record = frame_record(light);
    CY_CHECK_EQ(record.kind, rendering::kGpuLightEmitter);
    // The reach covers the room: a lamp this bright lights well past the slab's edge.
    CY_CHECK_GT(light.range, 10.0F);

    // The SAME light as an ordinary spot, the cone squared: what the frame drew without
    // `cosine_falloff`. It must be measurably wrong off the axis, or this case proves nothing.
    render::LightDescription squared = light;
    squared.cosine_falloff = false;
    const rendering::GpuLight squared_record = frame_record(squared);

    f64 error_total = 0.0;
    f32 error_worst = 0.0F;
    f32 squared_worst = 0.0F;
    u32 receivers = 0;
    for (u32 ring = 0; ring < 8; ++ring) {
        for (u32 around = 0; around < 8; ++around) {
            // The floor two metres below and the walls of a room about it: receivers from straight
            // under the lamp to sixty-five degrees off its axis, at 2 to 5 m.
            const f32 radius = 0.5F * static_cast<f32>(ring);
            const f32 turn = 0.785398F * static_cast<f32>(around);
            const Vec3 receiver =
                lamp.position + Vec3{radius * std::cos(turn),
                                     -2.0F - (0.25F * static_cast<f32>(ring % 3U)),
                                     radius * std::sin(turn)};
            const Vec3 normal{0.0F, 1.0F, 0.0F};
            const Vec3 gi_direct = gi::direct_radiance(lamp, receiver, normal);
            const f32 gi_luminance = gi::emission_luminance(gi_direct);
            const f32 peak = std::max({lamp.colour.x, lamp.colour.y, lamp.colour.z});
            const f32 frame_value = frame_illuminance(record, receiver, normal) *
                                    gi::emission_luminance(lamp.colour / peak) /
                                    std::numbers::pi_v<f32>;
            const f32 squared_value = frame_illuminance(squared_record, receiver, normal) *
                                      gi::emission_luminance(lamp.colour / peak) /
                                      std::numbers::pi_v<f32>;
            const f32 error = std::abs(frame_value - gi_luminance) / gi_luminance;
            error_total += static_cast<f64>(error);
            error_worst = std::max(error_worst, error);
            squared_worst =
                std::max(squared_worst, std::abs(squared_value - gi_luminance) / gi_luminance);
            receivers += 1;
        }
    }
    const f64 error_mean = error_total / static_cast<f64>(receivers);
    std::fprintf(
        stderr,
        "a promoted lamp in the frame against the GI's form factor over %u receivers: mean "
        "%.4f, worst %.4f relative; as a squared-cone spot, worst %.3f\n",
        receivers, error_mean, static_cast<f64>(error_worst), static_cast<f64>(squared_worst));
    CY_CHECK_LT(error_mean, 0.01);
    CY_CHECK_LT(error_worst, 0.03F);
    CY_CHECK_GT(squared_worst, 0.3F);
}

CY_TEST_CASE("(g) the frame's own lights carry the lamps the GI promoted") {
    // A system that promoted a lamp hands it to the frame; one that keeps it in the cache does not.
    gi::IlluminationSystem system;
    gi::IlluminationSettings settings;
    settings.field.levels = 1;
    settings.field.resolution = 16;
    settings.probes.levels = 1;
    settings.probes.half_extent_probes = 2;
    CY_REQUIRE(system.configure(settings).has_value());
    const gi::GiLight lamp = promoted_lamp();
    gi::Surfel card;
    card.position = lamp.position;
    card.normal = lamp.emitter_normal;
    card.area = lamp.emitter_area;
    card.emission = lamp.colour;
    card.instance_id = 1;
    card.material_id = 1;
    CY_REQUIRE(system
                   .ingest_cell(5, Aabb::from_center_extents(lamp.position, Vec3{1.0F, 1.0F, 1.0F}),
                                {&card, 1}, 0)
                   .has_value());
    CY_REQUIRE_EQ(system.emitters().lights().size(), 1U);
    Array<render::LightDescription> lights(allocator());
    CY_CHECK_EQ(rendering::gi_gpu::append_promoted_frame_lights(system, lights), 1U);
    CY_REQUIRE_EQ(lights.size(), 1U);
    CY_CHECK(lights[0].cosine_falloff);
    CY_CHECK_NEAR(lights[0].transform.translation.y, lamp.position.y, 1.0e-6F);
    CY_CHECK_NEAR(lights[0].transform.forward().y, -1.0F, 1.0e-5F);

    const gi::EmitterRule in_cache{gi::kAnyEmitter, gi::kAnyEmitter,
                                   gi::EmitterOverride::CacheOnly};
    CY_REQUIRE(system.set_emitter_rules({&in_cache, 1}, 1).has_value());
    lights.clear();
    CY_CHECK_EQ(rendering::gi_gpu::append_promoted_frame_lights(system, lights), 0U);
}

CY_TEST_CASE("(h) on the device, the promoted lamp lights the frame, falling off as the cosine") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // The scene lit by the lamp alone, so its light is the picture: without it the floor is the
    // frame's ambient only.
    const render::LightDescription lamp =
        rendering::gi_gpu::promoted_frame_light(promoted_lamp(), 1.0e-3F, false);
    render::LightDescription squared = lamp;
    squared.cosine_falloff = false;
    const std::vector<render::LightDescription> none;
    const std::vector<render::LightDescription> with_lamp{lamp};
    const std::vector<render::LightDescription> with_squared{squared};
    GiFrameRun dark(fixture, false, false, gi::GiMode::Dynamic, false, gi::GiGather::ScreenProbes,
                    &none);
    CY_REQUIRE(dark.render(3));
    GiFrameRun lit(fixture, false, false, gi::GiMode::Dynamic, false, gi::GiGather::ScreenProbes,
                   &with_lamp);
    CY_REQUIRE(lit.render(3));
    GiFrameRun cone(fixture, false, false, gi::GiMode::Dynamic, false, gi::GiGather::ScreenProbes,
                    &with_squared);
    CY_REQUIRE(cone.render(3));
    const auto luminance = [](u32 pixel) {
        return (0.2126 * static_cast<f64>(pixel & 0xFFU)) +
               (0.7152 * static_cast<f64>((pixel >> 8U) & 0xFFU)) +
               (0.0722 * static_cast<f64>((pixel >> 16U) & 0xFFU));
    };
    f64 dark_sum = 0.0;
    f64 lit_sum = 0.0;
    f64 cone_sum = 0.0;
    usize brighter = 0;
    usize dimmer_than_cone = 0;
    for (usize index = 0; index < lit.pixels().size(); ++index) {
        const f64 off = luminance(dark.pixels()[index]);
        const f64 on = luminance(lit.pixels()[index]);
        const f64 squared_on = luminance(cone.pixels()[index]);
        dark_sum += off;
        lit_sum += on;
        cone_sum += squared_on;
        brighter += on > off ? 1U : 0U;
        dimmer_than_cone += on + 1.0 < squared_on ? 1U : 0U;
    }
    std::fprintf(
        stderr,
        "the promoted lamp brightens %zu of %u pixels; mean luminance %.2f dark, %.2f lit, "
        "%.2f as a squared-cone spot\n",
        brighter, kWidth * kHeight, dark_sum / static_cast<f64>(kWidth * kHeight),
        lit_sum / static_cast<f64>(kWidth * kHeight),
        cone_sum / static_cast<f64>(kWidth * kHeight));
    write_picture(lit.pixels(), "gi_frame_promoted_lamp.png");
    CY_CHECK_GT(brighter, static_cast<usize>(kWidth * kHeight / 10U));
    // The cosine is at least its square everywhere, and above it off the axis: no pixel the
    // emitter lights is dimmer than the squared cone draws it, and the frame is brighter overall.
    CY_CHECK_EQ(dimmer_than_cone, 0U);
    CY_CHECK_GT(lit_sum, cone_sum);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
