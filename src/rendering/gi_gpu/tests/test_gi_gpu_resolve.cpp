// SPDX-License-Identifier: MIT
// The per-pixel resolve and its denoiser on the device, against the host implementation they
// transcribe, and what they cost at 1080p. Issue #35, stage 4.
//
// ================================================================================================
// WHAT THIS SUITE ASSERTS
// ================================================================================================
//
//   the resolve     `GpuScreenResolve`'s noisy diffuse and specular against
//   `ScreenResolve::produce`
//                   over the same surfaces, the same radiance cache, the same field and cards, the
//                   same frame — through the software tier, and through the hardware tier with ray
//                   queries over the structures `cy::rendering-raytracing-gpu` built;
//   the history     the reprojection against `screen_history`, pixel for pixel;
//   the denoiser    the accumulated and filtered signals against `denoise::Denoiser::denoise` run
//                   over the host's noisy input, frame after frame — the temporal half and the
//                   spatial half, which is GTAO's pattern with the history GTAO does not keep;
//   the cost        every stage at 1920x1080 on this device, both tiers, timed with timestamps
//                   around the dispatches, and the ray density the `TracingResolution` lever buys.
//
// The quality of the image — against the path tracer, behind a 10 cm wall, after a light moves,
// on a still view — is `integration.render_gi_screen`'s, measured on the host oracle this suite
// holds the device to; the bounds below are what makes that transfer.
//
// FLOATING POINT is bounded as stage 3's suites bound it: a driver may fuse a multiply and an add,
// and the device's `cos` and `exp` are not the host's, so the bounds are on the fraction of pixels
// beyond a small tolerance and on the typical difference, printed beside each.

#include <cy/backends/rhi/device.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/arbiter/arbiter.h>
#include <cy/rendering/arbiter/profiles.h>
#include <cy/rendering/gi/screen_resolve.h>
#include <cy/rendering/gi_gpu/gpu_budget.h>
#include <cy/rendering/gi_gpu/gpu_radiance_cache.h>
#include <cy/rendering/gi_gpu/gpu_resolve.h>
#include <cy/rendering/gi_gpu/gpu_scene.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/raytracing/capability.h>
#include <cy/rendering/raytracing_gpu/device_structures.h>
#include <cy/test/test.h>

#include "device.h"
#include "screen_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace cy;
namespace gi = cy::rendering::gi;
namespace rt = cy::rendering::rt;
namespace rt_gpu = cy::rendering::rt_gpu;
using cy::rendering::apply_profile;
using cy::rendering::ArbiterReport;
using cy::rendering::BudgetArbiter;
using cy::rendering::BudgetSubsystem;
using cy::rendering::cost_source_name;
using cy::rendering::CostSource;
using cy::rendering::named_profile;
using cy::rendering::ProfileName;
using cy::rendering::QualityLadder;
using cy::rendering::gi_gpu::GpuGiScene;
using cy::rendering::gi_gpu::GpuGiSceneDescription;
using cy::rendering::gi_gpu::GpuRadianceCache;
using cy::rendering::gi_gpu::GpuRadianceCacheDescription;
using cy::rendering::gi_gpu::GpuResolveTimes;
using cy::rendering::gi_gpu::GpuResolveView;
using cy::rendering::gi_gpu::GpuScreenResolve;
using cy::rendering::gi_gpu::GpuScreenResolveDescription;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// A device, or a loud skip. The message is the one the suite's SKIP_REGULAR_EXPRESSION matches.
struct Gpu {
    render_test::DeviceFixture fixture;

    explicit Gpu(bool ray_tracing = true)
        : fixture("vulkan", "render.gi_gpu_resolve", true, ray_tracing) {}

    [[nodiscard]] bool available() const noexcept {
        if (fixture.is(rhi::BackendKind::Vulkan) && GpuScreenResolve::supported(fixture.device())) {
            return true;
        }
        std::fprintf(stderr, "no requested graphics device on this machine; ");
        fixture.report_skip();
        return false;
    }

    [[nodiscard]] bool ray_queries() const noexcept {
        if (rt_gpu::DeviceStructures::supported(fixture.device())) {
            return true;
        }
        std::fprintf(stderr,
                     "this device reports no ray queries (%s); the hardware-tier case is "
                     "skipped and the software cases still run\n",
                     rt::ray_tracing_refusal(fixture.device().capabilities()));
        return false;
    }

    [[nodiscard]] rhi::Device& device() const noexcept { return fixture.device(); }
};

template <class Declare>
[[nodiscard]] bool run_frame(rhi::Device& device, Declare&& declare) {
    auto begun = device.begin_frame();
    if (!begun.has_value()) {
        std::fprintf(stderr, "begin_frame failed: %s\n", begun.error().message);
        return false;
    }
    rendering::RenderGraph graph(allocator());
    if (Status declared = declare(graph); !declared) {
        std::fprintf(stderr, "declare failed: %s\n", declared.error().message);
        (void)device.end_frame();
        return false;
    }
    rendering::GraphExecutor executor(allocator(), device);
    auto executed =
        executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{});
    if (!executed.has_value()) {
        std::fprintf(stderr, "execute failed: %s\n", executed.error().message);
        (void)device.end_frame();
        return false;
    }
    if (Status idle = device.wait_idle(); !idle) {
        std::fprintf(stderr, "wait_idle failed: %s\n", idle.error().message);
        (void)device.end_frame();
        return false;
    }
    return device.end_frame().has_value();
}

/// How closely two sets of answers agree: the mean and worst relative difference, and how many
/// exceed a stated tolerance.
struct Agreement {
    u32 compared = 0;
    u32 beyond = 0;
    f64 mean = 0.0;
    f64 worst = 0.0;

    void add(Vec3 a, Vec3 b, f64 tolerance) noexcept {
        const f64 scale =
            std::max({static_cast<f64>(length(a)), static_cast<f64>(length(b)), 1.0e-3});
        const f64 difference = static_cast<f64>(length(a - b)) / scale;
        mean += difference;
        worst = std::max(worst, difference);
        beyond += difference > tolerance ? 1U : 0U;
        compared += 1;
    }
    void finish() noexcept { mean = compared == 0 ? 0.0 : mean / compared; }
    void print(const char* what) const {
        std::fprintf(stderr, "%s: %u pixels, mean relative %.3g, worst %.3g, %u beyond\n", what,
                     compared, mean, worst, beyond);
    }
    [[nodiscard]] f64 fraction_beyond() const noexcept {
        return compared == 0 ? 0.0 : static_cast<f64>(beyond) / compared;
    }
};

/// Triangles for the scene's quads, both windings — the service culls nothing, but the quads are
/// two-sided and so are these.
struct QuadTriangles {
    std::vector<Vec3> positions;
    std::vector<u32> indices;

    explicit QuadTriangles(const std::vector<gi_support::AxisQuad>& quads) {
        for (const gi_support::AxisQuad& quad : quads) {
            const u32 a = quad.axis == 0 ? 1U : 0U;
            const u32 b = quad.axis == 2 ? 1U : 2U;
            const auto corner = [&](f32 u, f32 v) {
                f32 p[3] = {0.0F, 0.0F, 0.0F};
                p[quad.axis] = quad.coordinate;
                p[a] = u;
                p[b] = v;
                return Vec3{p[0], p[1], p[2]};
            };
            const auto base = static_cast<u32>(positions.size());
            positions.push_back(corner(quad.lo[0], quad.lo[1]));
            positions.push_back(corner(quad.hi[0], quad.lo[1]));
            positions.push_back(corner(quad.hi[0], quad.hi[1]));
            positions.push_back(corner(quad.lo[0], quad.hi[1]));
            indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }

    [[nodiscard]] rt::TriangleGeometry geometry() const noexcept {
        rt::TriangleGeometry triangles;
        triangles.positions = {positions.data(), positions.size()};
        triangles.indices = {indices.data(), indices.size()};
        return triangles;
    }
};

[[nodiscard]] GpuGiSceneDescription scene_description() noexcept {
    GpuGiSceneDescription description;
    description.max_levels = 2;
    description.max_window_bricks = 16;
    description.max_brick_slots = 16384;
    description.max_cards = 2048;
    description.max_selection = 2048;
    return description;
}

/// The device half beside a host `ScreenScene`: the scene, the probe set, the resolve and — for
/// the hardware tier — the structures, every input mirrored from the host's own state each frame.
struct DeviceResolve {
    GpuGiScene scene;
    GpuRadianceCache cache;
    GpuScreenResolve resolve;
    rt_gpu::DeviceStructures structures;
    QuadTriangles* triangles = nullptr;
    bool hardware = false;

    [[nodiscard]] bool create(rhi::Device& device, gi_support::ScreenScene& host,
                              bool with_hardware, bool readback = true) {
        hardware = with_hardware;
        if (!scene.create(allocator(), device, scene_description()).has_value() ||
            !cache.create(allocator(), device, scene, GpuRadianceCacheDescription{}).has_value()) {
            return false;
        }
        cache.bind_field(host.system.field());
        cache.bind_host_surfaces(&host.system.surfaces());
        if (hardware) {
            if (!structures.create(allocator(), device, {}).has_value()) {
                return false;
            }
            host.system.acceleration().set_mirror(&structures);
            (void)host.system.acceleration().update();
            if (!run_frame(device, [&](rendering::RenderGraph& graph) {
                    return structures.declare(graph);
                })) {
                return false;
            }
            if (!cache.bind_hardware(&host.system.acceleration(), &structures).has_value()) {
                return false;
            }
        }
        GpuScreenResolveDescription description;
        description.width = host.view.width;
        description.height = host.view.height;
        description.readback = readback;
        if (!resolve.create(allocator(), device, cache, description).has_value()) {
            return false;
        }
        resolve.set_settings(host.settings);
        resolve.set_denoiser(host.denoiser);
        resolve.set_field(host.system.field());
        resolve.set_sky(host.system.sky_term());
        return true;
    }

    /// The device's frame `frame`, after the host's `step(frame)` updated the caches it reads.
    [[nodiscard]] bool frame(rhi::Device& device, gi_support::ScreenScene& host, u64 frame) {
        if (!resolve.sync(host.system.radiance()).has_value()) {
            return false;
        }
        resolve.set_cache_frame(frame);
        GpuResolveView view;
        view.camera = host.view.camera;
        view.world_to_clip = host.view.world_to_clip;
        if (!resolve.begin_frame(view, frame).has_value() ||
            !resolve.set_surfaces(host.view.surfaces()).has_value()) {
            return false;
        }
        return run_frame(device, [&](rendering::RenderGraph& graph) -> Status {
            if (hardware) {
                if (Status built = structures.declare(graph); !built) {
                    return built;
                }
            }
            return resolve.declare_from_surfaces(graph);
        });
    }
};

/// The room, the host, and the device beside it, run together for `frames` frames; every frame's
/// noisy, history and denoised buffers compared.
struct Comparison {
    Agreement noisy_diffuse;
    Agreement noisy_specular;
    Agreement denoised_diffuse;
    Agreement denoised_specular;
    /// The published signals on the frame's pixels: the upsample, at a divisor above one.
    Agreement published_diffuse;
    Agreement published_specular;
    u32 history_pixels = 0;
    u32 history_mismatches = 0;
    gi::RadianceSource tier = gi::RadianceSource::None;
};

[[nodiscard]] bool compare_room(Gpu& gpu, bool hardware, u32 frames, Comparison& out,
                                u32 divisor = 1) {
    gi_support::ScreenScene host;
    gi_support::make_room(host, gi_support::kScreenWidth, gi_support::kScreenHeight, hardware);
    if (divisor != 1) {
        // A RIPPLE OF A FEW DEGREES ON EVERY NORMAL, for the half grid. The room is flat, so
        // neighbouring normals agree exactly and the upsample's facing term — raised to its power
        // — would weigh nothing a transcription could get wrong; rippled, it weighs every tap.
        for (usize index = 0; index < host.view.normal.size(); ++index) {
            const u32 hash = gi::screen_hash(static_cast<u32>(index), 0, 11);
            const Vec3 ripple{gi::screen_unit(hash) - 0.5F,
                              gi::screen_unit(hash * 0x27220A95U) - 0.5F,
                              gi::screen_unit(hash * 0x9E3779B9U) - 0.5F};
            host.view.normal[index] = normalize(host.view.normal[index] + (ripple * 0.3F));
        }
        host.divisor = divisor;
        host.resize_grid();
    }
    std::unique_ptr<QuadTriangles> triangles;
    if (hardware) {
        triangles = std::make_unique<QuadTriangles>(host.quads);
        CY_REQUIRE(host.system.acceleration()
                       .declare_geometry(1, rt::adapt_static_mesh(triangles->geometry(), 0))
                       .has_value());
        rt::InstanceDescriptor instance;
        instance.geometry = 1;
        instance.instance_id = 1;
        CY_REQUIRE(host.system.acceleration().add_instance(instance).has_value());
    }
    DeviceResolve device;
    if (!device.create(gpu.device(), host, hardware) ||
        !device.resolve.set_divisor(divisor).has_value()) {
        std::fprintf(stderr, "the device resolve could not be created\n");
        return false;
    }
    const usize pixels = static_cast<usize>(host.grid.width()) * host.grid.height();
    const usize frame_pixels = host.view.depth.size();
    const cy::rendering::gi::ScreenSurfaces grid = host.grid.surfaces();
    std::vector<Vec3> diffuse(pixels);
    std::vector<Vec3> specular(pixels);
    std::vector<i32> source(pixels);
    std::vector<f32> confidence(pixels);
    for (u64 frame = 0; frame < frames; ++frame) {
        host.step(frame);
        if (!device.frame(gpu.device(), host, frame)) {
            return false;
        }
        out.tier = device.resolve.last_tier();
        const bool last = frame + 1 == frames;
        CY_REQUIRE(
            device.resolve.read_back_noisy({diffuse.data(), pixels}, {specular.data(), pixels})
                .has_value());
        for (usize index = 0; index < pixels; ++index) {
            if (grid.depth[index] <= 0.0F) {
                continue;
            }
            out.noisy_diffuse.add(diffuse[index], host.resolve.diffuse()[index], 1.0e-3);
            out.noisy_specular.add(specular[index], host.resolve.specular()[index], 1.0e-3);
        }
        CY_REQUIRE(
            device.resolve.read_back_history({source.data(), pixels}, {confidence.data(), pixels})
                .has_value());
        for (usize index = 0; index < pixels; ++index) {
            out.history_pixels += 1;
            out.history_mismatches +=
                source[index] != host.source[index] || confidence[index] != host.confidence[index]
                    ? 1U
                    : 0U;
        }
        CY_REQUIRE(
            device.resolve.read_back_denoised({diffuse.data(), pixels}, {specular.data(), pixels})
                .has_value());
        if (last) {
            for (usize index = 0; index < pixels; ++index) {
                if (grid.depth[index] <= 0.0F) {
                    continue;
                }
                out.denoised_diffuse.add(diffuse[index], host.grid_diffuse[index], 1.0e-2);
                out.denoised_specular.add(specular[index], host.grid_specular[index], 1.0e-2);
            }
            std::vector<Vec3> published_diffuse(frame_pixels);
            std::vector<Vec3> published_specular(frame_pixels);
            CY_REQUIRE(device.resolve
                           .read_back_published({published_diffuse.data(), frame_pixels},
                                                {published_specular.data(), frame_pixels})
                           .has_value());
            for (usize index = 0; index < frame_pixels; ++index) {
                if (host.view.depth[index] <= 0.0F) {
                    continue;
                }
                out.published_diffuse.add(published_diffuse[index], host.diffuse[index], 1.0e-2);
                out.published_specular.add(published_specular[index], host.specular[index], 1.0e-2);
            }
        }
    }
    if (hardware) {
        host.system.acceleration().set_mirror(nullptr);
    }
    out.noisy_diffuse.finish();
    out.noisy_specular.finish();
    out.denoised_diffuse.finish();
    out.denoised_specular.finish();
    out.published_diffuse.finish();
    out.published_specular.finish();
    return true;
}

void check(const Comparison& result, const char* tier) {
    std::fprintf(stderr, "%s tier:\n", tier);
    result.noisy_diffuse.print("  noisy diffuse, every frame");
    result.noisy_specular.print("  noisy specular, every frame");
    result.denoised_diffuse.print("  denoised diffuse, last frame");
    result.denoised_specular.print("  denoised specular, last frame");
    result.published_diffuse.print("  published diffuse, last frame");
    result.published_specular.print("  published specular, last frame");
    std::fprintf(stderr, "  history: %u of %u pixels differ\n", result.history_mismatches,
                 result.history_pixels);
    // The one-ray signal: a ray whose direction differs in its last bit can graze a different
    // side of an edge, so a handful of pixels may answer differently; the typical one may not.
    CY_CHECK_LT(result.noisy_diffuse.mean, 1.0e-3);
    CY_CHECK_LT(result.noisy_diffuse.fraction_beyond(), 0.01);
    CY_CHECK_LT(result.noisy_specular.mean, 1.0e-3);
    CY_CHECK_LT(result.noisy_specular.fraction_beyond(), 0.01);
    // The reprojection is exact arithmetic on both sides.
    CY_CHECK_EQ(result.history_mismatches, 0U);
    // Accumulated over the frames and filtered by the cascade.
    CY_CHECK_LT(result.denoised_diffuse.mean, 2.0e-3);
    CY_CHECK_LT(result.denoised_diffuse.fraction_beyond(), 0.01);
    CY_CHECK_LT(result.denoised_specular.mean, 2.0e-3);
    CY_CHECK_LT(result.denoised_specular.fraction_beyond(), 0.01);
    // And upsampled onto the frame's pixels: the publish is `screen_upsample`.
    CY_CHECK_LT(result.published_diffuse.mean, 2.0e-3);
    CY_CHECK_LT(result.published_diffuse.fraction_beyond(), 0.01);
    CY_CHECK_LT(result.published_specular.mean, 2.0e-3);
    CY_CHECK_LT(result.published_specular.fraction_beyond(), 0.01);
}

}  // namespace

CY_TEST_CASE("the device resolve and denoiser are the host's, software tier") {
    Gpu gpu(false);
    if (!gpu.available()) {
        return;
    }
    // FORTY FRAMES, past the longest history either signal keeps (the indirect diffuse's 32): the
    // accumulation's floor on its blend weight is reached only there, and a transcription that
    // floored it wrongly agrees with the host for every frame before it.
    Comparison result;
    CY_REQUIRE(compare_room(gpu, false, 40, result));
    CY_CHECK_EQ(result.tier, gi::RadianceSource::SoftwareTrace);
    check(result, "software");
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("on a half-resolution grid the device resolve, denoiser and upsample are the host's") {
    Gpu gpu(false);
    if (!gpu.available()) {
        return;
    }
    Comparison result;
    CY_REQUIRE(compare_room(gpu, false, 8, result, 2));
    check(result, "software, divisor 2");
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the device resolve and denoiser are the host's, hardware tier with ray queries") {
    Gpu gpu(true);
    if (!gpu.available() || !gpu.ray_queries()) {
        return;
    }
    Comparison result;
    CY_REQUIRE(compare_room(gpu, true, 8, result));
    CY_CHECK_EQ(result.tier, gi::RadianceSource::HardwareTrace);
    check(result, "hardware");
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("what stage 4 costs on this device at 1920x1080, per stage, per tier and per lever") {
    Gpu gpu(true);
    if (!gpu.available()) {
        return;
    }
    const bool hardware_available = gpu.ray_queries();
    // The GI budget's two device levers at their positions: `TracingResolution`'s value and the
    // denoiser's quality position (`DenoiserQuality`).
    struct Run {
        const char* name;
        bool hardware;
        f32 tracing;
        u32 denoiser;
    };
    const Run runs[] = {
        {"software tier, authored quality", false, 1.0F, 0},
        {"hardware tier, authored quality", true, 1.0F, 0},
        {"hardware tier, TracingResolution 0.71 (a quarter of the pixels, upsampled)", true, 0.71F,
         0},
        {"hardware tier, TracingResolution 0.5 (and half their rays)", true, 0.5F, 0},
        {"hardware tier, TracingResolution 0.5 and DenoiserQuality 2", true, 0.5F, 2},
        {"software tier, TracingResolution 0.5 and DenoiserQuality 2", false, 0.5F, 2},
    };
    for (const Run& run : runs) {
        if (run.hardware && !hardware_available) {
            continue;
        }
        gi_support::ScreenScene host;
        gi_support::make_room(host, 1920, 1080, run.hardware, false);
        std::unique_ptr<QuadTriangles> triangles;
        if (run.hardware) {
            triangles = std::make_unique<QuadTriangles>(host.quads);
            CY_REQUIRE(host.system.acceleration()
                           .declare_geometry(1, rt::adapt_static_mesh(triangles->geometry(), 0))
                           .has_value());
            rt::InstanceDescriptor instance;
            instance.geometry = 1;
            instance.instance_id = 1;
            CY_REQUIRE(host.system.acceleration().add_instance(instance).has_value());
        }
        // The caches converge on the host; only the resolve's stages are timed.
        for (u64 frame = 0; frame < 6; ++frame) {
            gi::FrameContext context;
            context.camera = host.view.camera;
            context.lights = {host.lights.data(), host.lights.size()};
            context.frame = frame;
            (void)host.system.update(context);
        }
        host.settings.ray_density = cy::rendering::gi_gpu::tracing_density(run.tracing);
        host.denoiser.set_quality_position(run.denoiser);
        DeviceResolve device;
        CY_REQUIRE(device.create(gpu.device(), host, run.hardware, false));
        CY_REQUIRE(device.resolve.set_divisor(cy::rendering::gi_gpu::tracing_divisor(run.tracing))
                       .has_value());
        device.resolve.set_timing(true);
        std::vector<GpuResolveTimes> times;
        for (u64 frame = 6; frame < 6 + 6; ++frame) {
            CY_REQUIRE(device.frame(gpu.device(), host, frame));
            const GpuResolveTimes measured = device.resolve.last_times();
            CY_REQUIRE(measured.valid);
            // The first frame carries the pipelines' first use; it is not the cost.
            if (frame >= 7) {
                times.push_back(measured);
            }
        }
        // THE FASTEST FRAME, not the median: a device another process shares time-slices its
        // work into ours, and a timestamp interval then counts the other process's dispatch. The
        // fastest of five is the frame nothing interrupted.
        std::ranges::sort(times, [](const GpuResolveTimes& a, const GpuResolveTimes& b) {
            return a.total_ms < b.total_ms;
        });
        const GpuResolveTimes& fastest = times.front();
        std::fprintf(stderr,
                     "1920x1080, %s: reproject %.3f, resolve %.3f, accumulate %.3f, variance "
                     "%.3f, filter %.3f, publish %.3f ms; total %.3f ms\n",
                     run.name, fastest.reproject_ms, fastest.resolve_ms, fastest.accumulate_ms,
                     fastest.variance_ms, fastest.filter_ms, fastest.publish_ms, fastest.total_ms);
        CY_CHECK_EQ(device.resolve.last_tier(), run.hardware ? gi::RadianceSource::HardwareTrace
                                                             : gi::RadianceSource::SoftwareTrace);
        // A ceiling, not the claim: the claim is the printed line. A frame at 60 Hz is 16.7 ms.
        CY_CHECK_LT(fastest.total_ms, 16.0);
        if (run.hardware) {
            host.system.acceleration().set_mirror(nullptr);
        }
    }
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

// --- Stage 5: the device cost into the GI budget, and the budget's levers back onto the device
// ----

namespace {

using cy::rendering::gi_gpu::GpuGiBudget;
using cy::rendering::gi_gpu::GpuGiCost;

/// One frame of the GI with its cost measured on the device and fed back: the system updates
/// against the last frame's device time, the levers reach the device resolve, and the resolve's
/// timestamps become the next frame's measurement.
struct BudgetedFrame {
    gi_support::ScreenScene& host;
    DeviceResolve& device;
    GpuGiBudget& link;
    gi::ScreenResolveSettings authored;

    [[nodiscard]] gi::IlluminationFrameReport step(rhi::Device& rhi_device, u64 frame,
                                                   bool permit) {
        gi::FrameContext context;
        context.camera = host.view.camera;
        context.lights = {host.lights.data(), host.lights.size()};
        context.frame = frame;
        context.measured_gi_ms = link.measured_ms();
        context.permit_relaxation = permit;
        const gi::IlluminationFrameReport report = host.system.update(context);
        cy::rendering::gi_gpu::apply_levers(host.system, authored, device.resolve);
        CY_REQUIRE(device.frame(rhi_device, host, frame));
        const GpuResolveTimes times = device.resolve.last_times();
        GpuGiCost cost;
        cost.resolve_ms = times.total_ms;
        cost.resolve_valid = times.valid;
        link.observe(cost);
        return report;
    }
};

[[nodiscard]] std::vector<u32> lever_positions(const gi::GiBudget& budget) {
    std::vector<u32> positions;
    positions.reserve(gi::kGiLeverCount);
    for (u32 lever = 0; lever < gi::kGiLeverCount; ++lever) {
        positions.push_back(budget.position(static_cast<gi::GiLever>(lever)));
    }
    return positions;
}

}  // namespace

CY_TEST_CASE(
    "over budget, the GI drops its levers in the declared order, its device cost falls "
    "with them, and it relaxes only with the arbiter's permission") {
    Gpu gpu(false);
    if (!gpu.available()) {
        return;
    }
    gi_support::ScreenScene host;
    gi_support::make_room(host, 960, 540, false, false);
    for (u64 frame = 0; frame < 4; ++frame) {
        gi::FrameContext context;
        context.camera = host.view.camera;
        context.lights = {host.lights.data(), host.lights.size()};
        context.frame = frame;
        (void)host.system.update(context);
    }
    DeviceResolve device;
    CY_REQUIRE(device.create(gpu.device(), host, false, false));
    device.resolve.set_timing(true);
    GpuGiBudget link;
    BudgetedFrame frames{host, device, link, host.settings};
    gi::GiBudget& budget = host.system.budget();

    // Authored quality, with room to spare: nothing moves.
    budget.set_allocation_ms(50.0F);
    u64 frame = 4;
    for (; frame < 8; ++frame) {
        (void)frames.step(gpu.device(), frame, false);
    }
    const f32 authored_ms = link.measured_ms();
    CY_CHECK(link.measured());
    for (const u32 position : lever_positions(budget)) {
        CY_CHECK_EQ(position, 0U);
    }

    // FORCED OVER BUDGET: an allocation a tenth of what the device measures. The levers go down in
    // ascending reduction order, one step at a time, and none moves before every lever ahead of
    // it in the order has nothing left to give.
    budget.set_allocation_ms(authored_ms * 0.1F);
    std::vector<u32> tightened;
    for (u32 count = 0; count < 40 && !budget.at_reserved_minimum(); ++count, ++frame) {
        const std::vector<u32> before = lever_positions(budget);
        (void)frames.step(gpu.device(), frame, false);
        const std::vector<u32> after = lever_positions(budget);
        // Within one update the steps were taken in ascending order too: unfold them so.
        std::vector<u32> moved;
        for (u32 lever = 0; lever < gi::kGiLeverCount; ++lever) {
            for (u32 step = before[lever]; step < after[lever]; ++step) {
                moved.push_back(lever);
            }
        }
        std::ranges::sort(moved, [&](u32 a, u32 b) {
            return budget.schedule(static_cast<gi::GiLever>(a)).reduction_order <
                   budget.schedule(static_cast<gi::GiLever>(b)).reduction_order;
        });
        tightened.insert(tightened.end(), moved.begin(), moved.end());
    }
    // Two more frames at the floor, so the measurement is of the reduced levers.
    for (u32 count = 0; count < 2; ++count, ++frame) {
        (void)frames.step(gpu.device(), frame, false);
    }
    const f32 reduced_ms = link.measured_ms();
    std::fprintf(stderr, "levers in the order they went down:");
    for (const u32 lever : tightened) {
        std::fprintf(stderr, " %s", gi::gi_lever_name(static_cast<gi::GiLever>(lever)));
    }
    std::fprintf(stderr,
                 "\nthe device GI at 960x540: %.3f ms at authored quality, %.3f ms with every "
                 "lever at its floor\n",
                 static_cast<double>(authored_ms), static_cast<double>(reduced_ms));
    CY_CHECK(budget.at_reserved_minimum());
    CY_REQUIRE_FALSE(tightened.empty());
    CY_CHECK_EQ(tightened.front(), static_cast<u32>(gi::GiLever::DenoiserQuality));
    for (usize index = 1; index < tightened.size(); ++index) {
        CY_CHECK_LE(budget.schedule(static_cast<gi::GiLever>(tightened[index - 1])).reduction_order,
                    budget.schedule(static_cast<gi::GiLever>(tightened[index])).reduction_order);
    }
    // The levers reached the device: its own cost fell with them.
    CY_CHECK_LT(reduced_ms, authored_ms * 0.6F);

    // RECOVERY IS THE ARBITER'S: the allocation comes back, and without the arbiter's grant
    // nothing relaxes however many frames pass.
    ArbiterReport report;
    report.registered[static_cast<u32>(BudgetSubsystem::GlobalIllumination)] = true;
    report.allocation_ms[static_cast<u32>(BudgetSubsystem::GlobalIllumination)] = 50.0F;
    const std::vector<u32> floor = lever_positions(budget);
    for (u32 count = 0; count < 12; ++count, ++frame) {
        link.apply(report, budget);
        const gi::IlluminationFrameReport update = frames.step(gpu.device(), frame, false);
        CY_CHECK_EQ(update.budget.levers_relaxed, 0U);
    }
    CY_CHECK(lever_positions(budget) == floor);
    // Granted, one step back a grant, the lever that went down last first.
    report.relax_granted[static_cast<u32>(BudgetSubsystem::GlobalIllumination)] = true;
    u32 relaxed = 0;
    gi::GiLever first = gi::GiLever::Count;
    for (u32 count = 0; count < 12; ++count, ++frame) {
        link.apply(report, budget);
        const gi::IlluminationFrameReport update = frames.step(gpu.device(), frame, false);
        CY_CHECK_LE(update.budget.levers_relaxed, 1U);
        if (update.budget.levers_relaxed == 1U && first == gi::GiLever::Count) {
            first = update.budget.last_lever;
        }
        relaxed += update.budget.levers_relaxed;
    }
    std::fprintf(stderr, "granted: %u steps back in 12 frames, the first %s\n", relaxed,
                 first == gi::GiLever::Count ? "none" : gi::gi_lever_name(first));
    CY_CHECK_GT(relaxed, 0U);
    CY_CHECK_EQ(static_cast<u32>(first), tightened.back());
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "the device GI reports its measured cost to the arbiter on the rung its levers add up to") {
    // No device: the link's arithmetic over a real arbiter, so the cost the arbiter sees is the
    // one the device measured and the rung is the one the GI budget's levers predict.
    BudgetArbiter arbiter;
    CY_REQUIRE(apply_profile(named_profile(ProfileName::Standard), arbiter).has_value());
    gi::GiBudget budget;
    budget.set_allocation_ms(4.0F);
    GpuGiBudget link;
    GpuGiCost cost;
    cost.resolve_ms = 2.5;
    cost.resolve_valid = true;
    // A measurement that moves is a measurement: the arbiter confirms the claim against the
    // numbers, never the claim alone.
    for (u32 frame = 0; frame < 4; ++frame) {
        cost.resolve_ms = 2.5 + (0.1 * frame);
        link.observe(cost);
        arbiter.report_frame_ms(12.0F);
        link.submit(arbiter, budget);
        (void)arbiter.update();
    }
    const ArbiterReport report = arbiter.update();
    const auto gi_index = static_cast<u32>(BudgetSubsystem::GlobalIllumination);
    std::fprintf(stderr, "arbiter: GI measured %.3f ms (%s), allocated %.3f ms\n",
                 static_cast<double>(report.measured_ms[gi_index]),
                 cost_source_name(report.cost_source[gi_index]),
                 static_cast<double>(report.allocation_ms[gi_index]));
    CY_CHECK_EQ(report.cost_source[gi_index], CostSource::Measured);
    CY_CHECK_GT(report.measured_ms[gi_index], 2.4F);
    const QualityLadder& ladder = arbiter.declaration(BudgetSubsystem::GlobalIllumination)->ladder;
    CY_CHECK_EQ(cy::rendering::gi_gpu::arbiter_position(budget, ladder), 0U);
    // Every lever at its floor predicts the bottom of the declared ladder.
    for (u32 count = 0; count < 20; ++count) {
        (void)budget.update(100.0F, count);
    }
    CY_CHECK(budget.at_reserved_minimum());
    CY_CHECK_EQ(cy::rendering::gi_gpu::arbiter_position(budget, ladder), ladder.last_position());
}
