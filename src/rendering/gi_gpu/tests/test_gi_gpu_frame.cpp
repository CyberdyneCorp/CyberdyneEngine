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

#include "frame_scene.h"
#include "golden.h"
#include "screen_support.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/gi_gpu/gpu_radiance_cache.h>
#include <cy/rendering/gi_gpu/gpu_resolve.h>
#include <cy/rendering/gi_gpu/gpu_scene.h>
#include <cy/test/test.h>

#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
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
        std::fprintf(stderr,
                     "no requested graphics device on this machine; the backend selected was '%s'\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

/// The pipeline scene's boxes as the GI scene: a field of boxes, a card every half metre over every
/// face, and the scene's own sun and two point lights.
struct GiOfScene {
    std::vector<std::unique_ptr<gi_support::BoxField>> fields;
    std::vector<gi::Surfel> surfels;
    std::vector<gi::GiLight> lights;
    gi::IlluminationSystem system;

    [[nodiscard]] bool build(Span<const Aabb> boxes) {
        gi::IlluminationSettings settings;
        settings.field.levels = 2;
        settings.field.resolution = 64;
        settings.field.base_extent_metres = 16.0F;
        settings.probes.levels = 2;
        settings.probes.base_spacing_metres = 1.0F;
        settings.probes.half_extent_probes = 5;
        settings.rays_per_probe = 16;
        settings.max_ray_distance_metres = 24.0F;
        settings.surface_bounce = gi::SurfaceBounce::Cards;
        settings.card_gather.max_distance_metres = 24.0F;
        // The sky the frame's ambient term comes from, in the same physical units: an overcast
        // dome of a few thousand nits under a 22 000 lux sun.
        settings.sky.intensity = 2500.0F;
        if (!system.configure(settings).has_value()) {
            return false;
        }
        std::vector<gi_support::AxisQuad> quads;
        u64 id = 1;
        for (const Aabb& box : boxes) {
            const Vec3 centre = box.center();
            const Vec3 half = box.half_extents();
            const u32 grid = std::max(9U, static_cast<u32>(std::ceil(
                                              (std::max({half.x, half.y, half.z}) + 0.5F) * 6.0F)));
            fields.push_back(std::make_unique<gi_support::BoxField>(half, 0.5F, grid | 1U));
            if (!system.field()
                     .place(id++, fields.back()->asset(), Mat4::from_translation(centre))
                     .has_value()) {
                return false;
            }
            const Vec3 lo = box.min;
            const Vec3 hi = box.max;
            const Vec3 grey{0.62F, 0.6F, 0.58F};
            quads.push_back({0, lo.x, {lo.y, lo.z}, {hi.y, hi.z}, grey, -1.0F});
            quads.push_back({0, hi.x, {lo.y, lo.z}, {hi.y, hi.z}, grey, 1.0F});
            quads.push_back({1, lo.y, {lo.x, lo.z}, {hi.x, hi.z}, grey, -1.0F});
            quads.push_back({1, hi.y, {lo.x, lo.z}, {hi.x, hi.z}, grey, 1.0F});
            quads.push_back({2, lo.z, {lo.x, lo.y}, {hi.x, hi.y}, grey, -1.0F});
            quads.push_back({2, hi.z, {lo.x, lo.y}, {hi.x, hi.y}, grey, 1.0F});
        }
        surfels = gi_support::surfels_from_quads(quads, 0.5F);
        const Aabb bounds = Aabb::from_center_extents(Vec3{0.0F, 0.0F, -8.0F},
                                                      Vec3{12.0F, 6.0F, 12.0F});
        if (!system.ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 0).has_value()) {
            return false;
        }
        system.surfaces().set_lookup_radius(0.4F);

        gi::GiLight sun;
        sun.directional = true;
        sun.direction = normalize(Vec3{-0.28F, -0.82F, -0.50F});
        sun.colour = Vec3{1.0F, 0.96F, 0.88F};
        sun.intensity = 22000.0F;
        sun.id = 1;
        lights.push_back(sun);
        for (u32 which = 1; which < 3U; ++which) {
            gi::GiLight point;
            point.position = Vec3{which == 1U ? -3.2F : 3.2F, 1.4F, -5.4F};
            point.colour =
                which == 1U ? Vec3{0.4F, 0.7F, 1.0F} : Vec3{1.0F, 0.5F, 0.3F};
            point.intensity = 9000.0F;
            point.range = 16.0F;
            point.id = 1 + which;
            lights.push_back(point);
        }
        return true;
    }
};

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
    u64 frame = 0;
};

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
    frame->resolve->set_jitter(Vec2{upload.view.temporal_jitter[0], upload.view.temporal_jitter[1]});
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
    rendering::gi_gpu::write_gi_control(kDiffuseSlot, kSpecularSlot, frame->mode,
                                        upload.view.gi_control);
    return ok();
}

/// A scene rendered `frames` times with a case's hooks; the last frame's pixels kept.
class GiFrameRun {
public:
    GiFrameRun(DeviceFixture& fixture, bool attached, bool bound, gi::GiMode mode)
        : scene_(allocator()) {
        case_.scene = &scene_;
        case_.gi = &gi_;
        case_.resolve = &resolve_;
        case_.attached = attached;
        case_.bound = bound;
        case_.mode = mode;
        FrameSceneHooks hooks;
        hooks.user = &case_;
        hooks.configure = &configure_case;
        hooks.before_assemble = &before_assemble;
        hooks.before_upload = &before_upload;
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
        ready_ = gi_.build(scene_.boxes()) &&
                 gscene_.create(allocator(), fixture.device(), description).has_value() &&
                 cache_.create(allocator(), fixture.device(), gscene_,
                               GpuRadianceCacheDescription{})
                     .has_value();
        if (ready_) {
            cache_.bind_field(gi_.system.field());
            cache_.bind_host_surfaces(&gi_.system.surfaces());
            ready_ = resolve_.create(allocator(), fixture.device(), cache_, resolve).has_value();
        }
        if (ready_) {
            gi::ScreenResolveSettings settings;
            settings.mode = mode;
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
    const usize changed = differing(off.pixels(), on.pixels());
    std::fprintf(stderr, "GI on changes %zu of %u pixels; under Baked it changes %zu\n", changed,
                 kWidth * kHeight, differing(off.pixels(), excluded.pixels()));
    CY_CHECK_GT(changed, static_cast<usize>(kWidth * kHeight / 10U));
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
