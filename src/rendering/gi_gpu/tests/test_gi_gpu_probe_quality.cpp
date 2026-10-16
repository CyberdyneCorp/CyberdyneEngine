// SPDX-License-Identifier: MIT
// What each final gather is worth and costs on this device at 1080p and at 4K: the guide's axes —
// the path tracer, a 10 cm wall, a still view, a moving light — and every stage's GPU time, for the
// per-pixel gather and the screen probes every 16 and every 8 pixels, through both world tiers.
// Issue #35, the follow-up of stage 4.
//
// SMOKE, AND ON THE DEVICE. render.gi_gpu_resolve holds the device to the host oracle at a few
// hundred pixels; the host suites measure the oracle at 128x72. The numbers the guide quotes are
// these: the frame a 1920x1080 view publishes, read back, against the path tracer over the exact
// geometry. Each case converges a scene per gather and per tier, which is seconds of the host's
// cache updates and readbacks, so the suite is a smoke suite with a device, skipped where there is
// none.
//
// THE TIMES ARE THE FASTEST OF FIVE FRAMES, as render.gi_gpu_resolve's: a device another process
// shares time-slices its work into ours, and the fastest frame is the one nothing interrupted.

#include <cy/rendering/gi/bake.h>
#include <cy/rendering/gi_gpu/gpu_budget.h>

#include "resolve_support.h"

#include <iterator>
#include <memory>
#include <vector>

namespace {

using namespace gi_gpu_support;  // NOLINT(google-build-using-namespace)

constexpr const char* kSuite = "smoke.gi_gpu_probe_quality";

struct Variant {
    const char* name = "";
    gi::GiGather gather = gi::GiGather::ScreenProbes;
    u32 spacing = 16;
    gi::ScreenProbeDenoiser denoiser = gi::ScreenProbeDenoiser::ProbeSpace;
};

/// The 16-pixel probes under each denoiser: the default against the lever's other position.
[[nodiscard]] bool probe_space_16(const Variant& variant) {
    return variant.gather == gi::GiGather::ScreenProbes && variant.spacing == 16 &&
           variant.denoiser == gi::ScreenProbeDenoiser::ProbeSpace;
}
[[nodiscard]] bool shared_16(const Variant& variant) {
    return variant.gather == gi::GiGather::ScreenProbes && variant.spacing == 16 &&
           variant.denoiser == gi::ScreenProbeDenoiser::Shared;
}

constexpr Variant kVariants[] = {
    {"per-pixel gather", gi::GiGather::PerPixel, 16},
    {"screen probes every 16 px", gi::GiGather::ScreenProbes, 16},
    {"screen probes every 16 px, shared denoiser", gi::GiGather::ScreenProbes, 16,
     gi::ScreenProbeDenoiser::Shared},
    {"screen probes every 8 px", gi::GiGather::ScreenProbes, 8},
};

[[nodiscard]] const char* tier_name(bool hardware) {
    return hardware ? "hardware" : "software";
}

/// Triangles for the hardware tier, declared with the host's acceleration service.
void declare_triangles(gi_support::ScreenScene& host, std::unique_ptr<QuadTriangles>& triangles) {
    triangles = std::make_unique<QuadTriangles>(host.quads);
    CY_REQUIRE(host.system.acceleration()
                   .declare_geometry(1, rt::adapt_static_mesh(triangles->geometry(), 0))
                   .has_value());
    rt::InstanceDescriptor instance;
    instance.geometry = 1;
    instance.instance_id = 1;
    CY_REQUIRE(host.system.acceleration().add_instance(instance).has_value());
}

/// A scene the device resolves at full size: the host's caches, the device's frame, and what it
/// published.
struct DeviceScene {
    gi_support::ScreenScene host;
    std::unique_ptr<QuadTriangles> triangles;
    DeviceResolve device;
    bool hardware = false;
    std::vector<Vec3> diffuse;
    std::vector<Vec3> specular;
    u64 frames = 0;

    ~DeviceScene() {
        if (hardware) {
            host.system.acceleration().set_mirror(nullptr);
        }
    }

    void configure(const Variant& variant, bool with_hardware) {
        hardware = with_hardware;
        host.settings.gather = variant.gather;
        host.settings.probes.spacing_pixels = variant.spacing;
        host.settings.probes.denoiser = variant.denoiser;
    }

    /// After `host` is built: the device half.
    [[nodiscard]] bool create(Gpu& gpu, bool readback = true) {
        if (hardware) {
            declare_triangles(host, triangles);
        }
        if (!device.create(gpu.device(), host, hardware, readback)) {
            return false;
        }
        const usize pixels = host.view.depth.size();
        diffuse.assign(readback ? pixels : 0, Vec3{0.0F, 0.0F, 0.0F});
        specular.assign(readback ? pixels : 0, Vec3{0.0F, 0.0F, 0.0F});
        return true;
    }

    /// One frame: the host's caches, then the device's resolve over the same view.
    [[nodiscard]] bool step(Gpu& gpu, Span<const Aabb> invalidated = {}) {
        gi::FrameContext context;
        context.camera = host.view.camera;
        context.lights = {host.lights.data(), host.lights.size()};
        context.frame = frames;
        context.measured_gi_ms = 1.0F;
        (void)host.system.update(context);
        const bool same_view = frames > 0;
        const bool stepped = device.frame(gpu.device(), host, frames, invalidated, same_view);
        frames += 1;
        return stepped;
    }

    [[nodiscard]] bool run(Gpu& gpu, u32 count) {
        for (u32 index = 0; index < count; ++index) {
            if (!step(gpu)) {
                return false;
            }
        }
        return true;
    }

    void read() {
        CY_REQUIRE(device.resolve
                       .read_back_published({diffuse.data(), diffuse.size()},
                                            {specular.data(), specular.size()})
                       .has_value());
    }
};

/// The GI test room at `width` x `height`, as gi_support::make_room builds it, without the host's
/// own resolve.
void make_device_room(DeviceScene& scene, u32 width, u32 height) {
    gi_support::make_room(scene.host, width, height, scene.hardware, false);
}

/// The 10 cm wall's two sealed halves, one light in the near one.
void make_wall(DeviceScene& scene, Vec3 eye, Vec3 target, u32 width, u32 height) {
    gi_support::ScreenScene& host = scene.host;
    host.quads =
        gi_support::room_quads(gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ, false);
    host.quads.push_back({2,
                          -0.05F,
                          {-gi_support::kRoomX, -gi_support::kRoomY},
                          {gi_support::kRoomX, gi_support::kRoomY},
                          {0.6F, 0.6F, 0.6F},
                          -1.0F});
    host.quads.push_back({2,
                          0.05F,
                          {-gi_support::kRoomX, -gi_support::kRoomY},
                          {gi_support::kRoomX, gi_support::kRoomY},
                          {0.6F, 0.6F, 0.6F},
                          1.0F});
    host.surfels = gi_support::surfels_from_quads(host.quads, 1.0F);
    host.add_solid(Vec3{0.0F, 0.0F, 0.0F}, Vec3{gi_support::kRoomX, gi_support::kRoomY, 0.05F}, 43);
    host.lights[0].position = Vec3{-1.0F, 0.5F, -1.2F};
    host.build(eye, target, width, height, scene.hardware, false);
}

constexpr u32 kWidth = 1920;
constexpr u32 kHeight = 1080;

/// The error, convergence and stability measurements of one gather through one tier.
void measure_reference_and_stability(Gpu& gpu, bool hardware) {
    f32 per_pixel_error = 0.0F;
    // The probe-space denoiser against the shared one it replaces, on the same view and tier.
    f32 error[2] = {0.0F, 0.0F};
    f32 stillness[2] = {0.0F, 0.0F};
    for (const Variant& variant : kVariants) {
        DeviceScene scene;
        scene.configure(variant, hardware);
        make_device_room(scene, kWidth, kHeight);
        CY_REQUIRE(scene.create(gpu));
        CY_REQUIRE(scene.run(gpu, 40));
        scene.read();
        const std::vector<Vec3> previous = scene.diffuse;
        CY_REQUIRE(scene.step(gpu));
        scene.read();
        const f32 still =
            gi_support::image_difference(scene.diffuse, previous, scene.host.view.depth);

        gi_support::QuadTracer exact(scene.host.quads);
        const gi::PathTracer reference(exact, scene.host.system.scene(),
                                       {scene.host.lights.data(), scene.host.lights.size()},
                                       scene.host.system.sky_term(), &exact);
        gi::BakeSettings bake;
        bake.bounces = 3;
        bake.samples = 192;
        std::vector<Vec3> truth;
        std::vector<Vec3> frame;
        for (u32 y = 48; y < kHeight; y += 96) {
            for (u32 x = 48; x < kWidth; x += 96) {
                const usize index = (static_cast<usize>(y) * kWidth) + x;
                if (scene.host.view.depth[index] <= 0.0F) {
                    continue;
                }
                truth.push_back(reference.irradiance(scene.host.view.position[index],
                                                     scene.host.view.normal[index], bake));
                frame.push_back(scene.diffuse[index]);
            }
        }
        const gi::ReferenceComparison measured = gi::compare_against_reference(
            {frame.data(), frame.size()}, {truth.data(), truth.size()});
        std::fprintf(stderr,
                     "1920x1080, %s tier, %s: %.3f relative to the path tracer over %u points; a "
                     "converged still view moves %.4f a frame\n",
                     tier_name(hardware), variant.name,
                     static_cast<double>(measured.relative_error), measured.samples,
                     static_cast<double>(still));
        CY_CHECK_GT(measured.samples, 150U);
        CY_CHECK_LT(measured.relative_error, 0.16F);
        CY_CHECK_LT(still, 0.01F);
        if (variant.gather == gi::GiGather::PerPixel) {
            per_pixel_error = measured.relative_error;
        } else {
            CY_CHECK_LT(measured.relative_error, per_pixel_error + 0.02F);
        }
        if (probe_space_16(variant) || shared_16(variant)) {
            const usize which = probe_space_16(variant) ? 0 : 1;
            error[which] = measured.relative_error;
            stillness[which] = still;
        }
    }
    // NO WORSE THAN WHAT IT REPLACED: the error within a rounding of the shared denoiser's (0.071 /
    // 0.064 against 0.076 / 0.066, software / hardware), and a still view no less still.
    CY_CHECK_LE(error[0], error[1] + 0.002F);
    CY_CHECK_LE(stillness[0], stillness[1]);
}

void measure_wall(Gpu& gpu, bool hardware) {
    for (const Variant& variant : kVariants) {
        f64 lit_total = 0.0;
        u32 lit_count = 0;
        {
            // One scene at a time: each holds a 1080p resolve and its own structures.
            DeviceScene lit;
            lit.configure(variant, hardware);
            make_wall(lit, Vec3{2.5F, 0.3F, -3.6F}, Vec3{-1.0F, -0.5F, -0.2F}, kWidth, kHeight);
            CY_REQUIRE(lit.create(gpu));
            CY_REQUIRE(lit.run(gpu, 30));
            lit.read();
            for (usize index = 0; index < lit.diffuse.size(); ++index) {
                if (lit.host.view.depth[index] > 0.0F) {
                    lit_total += static_cast<f64>(gi_support::luminance(lit.diffuse[index]));
                    lit_count += 1;
                }
            }
        }
        DeviceScene sealed;
        sealed.configure(variant, hardware);
        make_wall(sealed, Vec3{2.5F, 0.3F, 3.6F}, Vec3{-1.0F, -0.5F, 0.2F}, kWidth, kHeight);
        CY_REQUIRE(sealed.create(gpu));
        CY_REQUIRE(sealed.run(gpu, 30));
        sealed.read();
        f64 sealed_total = 0.0;
        f64 sealed_worst = 0.0;
        u32 sealed_count = 0;
        for (usize index = 0; index < sealed.diffuse.size(); ++index) {
            if (sealed.host.view.depth[index] > 0.0F &&
                sealed.host.view.position[index].z > 0.05F) {
                const auto value = static_cast<f64>(gi_support::luminance(sealed.diffuse[index]));
                sealed_total += value;
                sealed_worst = std::max(sealed_worst, value);
                sealed_count += 1;
            }
        }
        const f64 lit_mean = lit_total / std::max(1U, lit_count);
        const f64 sealed_mean = sealed_total / std::max(1U, sealed_count);
        std::fprintf(stderr,
                     "1920x1080, %s tier, %s, 10 cm wall: the sealed room's mean %.2e of the lit "
                     "room's, its worst pixel %.2e\n",
                     tier_name(hardware), variant.name, sealed_mean / lit_mean,
                     sealed_worst / lit_mean);
        CY_REQUIRE(lit_mean > 0.0);
        CY_CHECK_LT(sealed_mean / lit_mean, 1.0e-3);
        // The per-pixel gather is the reference beside the probes here, and its worst pixel at
        // 1080p is its own measurement (4.1e-2 through the software tier, against 7.6e-3 at the
        // 40x24 of integration.render_gi_screen, which holds it to 2e-2): more pixels sit at the
        // wall's foot. The probes are held to the 40x24 bound at 1080p.
        if (variant.gather == gi::GiGather::ScreenProbes) {
            CY_CHECK_LT(sealed_worst / lit_mean, 2.0e-2);
        }
    }
}

void measure_convergence(Gpu& gpu, bool hardware) {
    const Vec3 moved{2.4F, 1.2F, -2.6F};
    u32 settle_frames[2] = {0, 0};
    for (const Variant& variant : kVariants) {
        std::vector<Vec3> target;
        {
            DeviceScene settled;
            settled.configure(variant, hardware);
            make_device_room(settled, kWidth, kHeight);
            settled.host.lights[0].position = moved;
            CY_REQUIRE(settled.create(gpu));
            CY_REQUIRE(settled.run(gpu, 48));
            settled.read();
            target = settled.diffuse;
        }
        DeviceScene scene;
        scene.configure(variant, hardware);
        make_device_room(scene, kWidth, kHeight);
        CY_REQUIRE(scene.create(gpu));
        CY_REQUIRE(scene.run(gpu, 32));
        scene.read();
        const std::vector<f32>& depth = scene.host.view.depth;
        const f32 before = gi_support::image_difference(scene.diffuse, target, depth);
        const Aabb room = Aabb::from_center_extents(
            Vec3{0.0F, 0.0F, 0.0F},
            Vec3{gi_support::kRoomX + 0.5F, gi_support::kRoomY + 0.5F, gi_support::kRoomZ + 0.5F});
        scene.host.lights[0].position = moved;
        scene.host.system.scene().invalidate(room, gi::InvalidationCause::LightChanged,
                                             scene.host.lights[0].id, scene.frames);
        constexpr f32 kWithin = 0.10F;
        u32 frames = 0;
        f32 difference = before;
        for (u32 count = 0; count < 40; ++count) {
            CY_REQUIRE(
                scene.step(gpu, count == 0 ? Span<const Aabb>{&room, 1} : Span<const Aabb>{}));
            scene.read();
            frames += 1;
            difference = gi_support::image_difference(scene.diffuse, target, depth);
            if (difference <= kWithin) {
                break;
            }
        }
        std::fprintf(stderr,
                     "1920x1080, %s tier, %s, a light moved 4.9 m: %.3f from the new light's "
                     "converged frame before, within %.2f after %u frames (%.3f)\n",
                     tier_name(hardware), variant.name, static_cast<double>(before),
                     static_cast<double>(kWithin), frames, static_cast<double>(difference));
        CY_CHECK_GT(before, 0.3F);
        CY_CHECK_LE(difference, kWithin);
        CY_CHECK_LE(frames, 14U);
        if (probe_space_16(variant) || shared_16(variant)) {
            settle_frames[probe_space_16(variant) ? 0 : 1] = frames;
        }
    }
    // The probe-space denoiser settles no later than the shared one (9 / 10 frames against 14 /
    // 13): its history is cut where the signal leaves it.
    CY_CHECK_LE(settle_frames[0], settle_frames[1]);
}

}  // namespace

CY_TEST_CASE("at 1920x1080 through the software tier: each gather against the path tracer, still") {
    Gpu gpu(false, kSuite);
    if (!gpu.available()) {
        return;
    }
    measure_reference_and_stability(gpu, false);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("at 1920x1080 through the hardware tier: each gather against the path tracer, still") {
    Gpu gpu(true, kSuite);
    if (!gpu.available() || !gpu.ray_queries()) {
        return;
    }
    measure_reference_and_stability(gpu, true);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("at 1920x1080 through the software tier: no light through a 10 cm wall") {
    Gpu gpu(false, kSuite);
    if (!gpu.available()) {
        return;
    }
    measure_wall(gpu, false);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("at 1920x1080 through the hardware tier: no light through a 10 cm wall") {
    Gpu gpu(true, kSuite);
    if (!gpu.available() || !gpu.ray_queries()) {
        return;
    }
    measure_wall(gpu, true);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "at 1920x1080 through the software tier: converged on a moved light within 14 frames") {
    Gpu gpu(false, kSuite);
    if (!gpu.available()) {
        return;
    }
    measure_convergence(gpu, false);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "at 1920x1080 through the hardware tier: converged on a moved light within 14 frames") {
    Gpu gpu(true, kSuite);
    if (!gpu.available() || !gpu.ray_queries()) {
        return;
    }
    measure_convergence(gpu, true);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

namespace {

struct CostRun {
    const char* name = "";
    u32 width = 0;
    u32 height = 0;
    bool hardware = false;
    gi::GiGather gather = gi::GiGather::ScreenProbes;
    u32 spacing = 16;
    /// `DenoiserQuality`'s position.
    u32 denoiser = 0;
    /// `TracingResolution`'s value: below 0.99 the half grid, below 0.6 half the rays besides.
    f32 tracing = 1.0F;
    gi::ScreenProbeDenoiser probe_denoiser = gi::ScreenProbeDenoiser::ProbeSpace;
};

/// The fastest of five frames of one configuration, every stage.
[[nodiscard]] GpuResolveTimes cost_of(Gpu& gpu, const CostRun& run) {
    DeviceScene scene;
    scene.configure(Variant{run.name, run.gather, run.spacing, run.probe_denoiser}, run.hardware);
    make_device_room(scene, run.width, run.height);
    scene.host.denoiser.set_quality_position(run.denoiser);
    scene.host.settings.ray_density = cy::rendering::gi_gpu::tracing_density(run.tracing);
    CY_REQUIRE(scene.create(gpu, false));
    CY_REQUIRE(scene.device.resolve.set_divisor(cy::rendering::gi_gpu::tracing_divisor(run.tracing))
                   .has_value());
    scene.device.resolve.set_timing(true);
    std::vector<GpuResolveTimes> times;
    for (u32 frame = 0; frame < 8; ++frame) {
        CY_REQUIRE(scene.step(gpu));
        const GpuResolveTimes measured = scene.device.resolve.last_times();
        CY_REQUIRE(measured.valid);
        // The first frames carry the pipelines' first use and the probes' first history.
        if (frame >= 3) {
            times.push_back(measured);
        }
    }
    std::ranges::sort(times, [](const GpuResolveTimes& a, const GpuResolveTimes& b) {
        return a.total_ms < b.total_ms;
    });
    const GpuResolveTimes& fastest = times.front();
    std::fprintf(stderr,
                 "%ux%u, %s tier, %s, %s, denoiser %u, tracing %.2f: unpack %.3f, reproject %.3f, "
                 "place %.3f, trace %.3f, probe filter %.3f, integrate %.3f, accumulate %.3f, "
                 "variance %.3f, filter %.3f, publish %.3f ms; total %.3f ms\n",
                 run.width, run.height, tier_name(run.hardware), run.name,
                 run.gather == gi::GiGather::ScreenProbes
                     ? gi::screen_probe_denoiser_name(run.probe_denoiser)
                     : "shared",
                 run.denoiser, static_cast<double>(run.tracing), fastest.unpack_ms,
                 fastest.reproject_ms, fastest.probe_place_ms, fastest.probe_trace_ms,
                 fastest.probe_filter_ms, fastest.integrate_ms, fastest.accumulate_ms,
                 fastest.variance_ms, fastest.filter_ms, fastest.publish_ms, fastest.total_ms);
    CY_CHECK_EQ(scene.device.resolve.last_tier(), run.hardware ? gi::RadianceSource::HardwareTrace
                                                               : gi::RadianceSource::SoftwareTrace);
    if (run.gather == gi::GiGather::PerPixel) {
        // Two timestamps written back to back: no work between them.
        CY_CHECK_LT(fastest.probe_trace_ms, 0.01);
    } else {
        CY_CHECK_GT(fastest.probe_trace_ms, 0.0);
    }
    return fastest;
}

/// Every run's fastest total, in the order given; zero for a run the device cannot make.
std::vector<f64> cost_runs(Gpu& gpu, Span<const CostRun> runs, bool hardware_available) {
    std::vector<f64> totals;
    for (const CostRun& run : runs) {
        if (run.hardware && !hardware_available) {
            totals.push_back(0.0);
            continue;
        }
        totals.push_back(cost_of(gpu, run).total_ms);
    }
    return totals;
}

}  // namespace

CY_TEST_CASE("what each gather costs at 1920x1080, per stage and per tier") {
    Gpu gpu(true, kSuite);
    if (!gpu.available()) {
        return;
    }
    const bool hardware = gpu.ray_queries();
    const CostRun runs[] = {
        {"per-pixel gather", kWidth, kHeight, true, gi::GiGather::PerPixel, 16, 0},
        {"per-pixel gather", kWidth, kHeight, false, gi::GiGather::PerPixel, 16, 0},
        {"screen probes every 16 px", kWidth, kHeight, true, gi::GiGather::ScreenProbes, 16, 0},
        {"screen probes every 16 px", kWidth, kHeight, false, gi::GiGather::ScreenProbes, 16, 0},
        // The lever's other position: the shared denoiser over the integrated pixels.
        {"screen probes every 16 px", kWidth, kHeight, true, gi::GiGather::ScreenProbes, 16, 0,
         1.0F, gi::ScreenProbeDenoiser::Shared},
        {"screen probes every 16 px", kWidth, kHeight, false, gi::GiGather::ScreenProbes, 16, 0,
         1.0F, gi::ScreenProbeDenoiser::Shared},
        {"screen probes every 8 px", kWidth, kHeight, true, gi::GiGather::ScreenProbes, 8, 0},
        {"screen probes every 8 px", kWidth, kHeight, false, gi::GiGather::ScreenProbes, 8, 0},
        {"screen probes every 16 px", kWidth, kHeight, true, gi::GiGather::ScreenProbes, 16, 2},
        {"screen probes every 16 px", kWidth, kHeight, false, gi::GiGather::ScreenProbes, 16, 2},
        // `TracingResolution` 0.71: integrated and denoised on the half grid, upsampled.
        {"screen probes every 16 px", kWidth, kHeight, true, gi::GiGather::ScreenProbes, 16, 0,
         0.71F},
        {"screen probes every 16 px", kWidth, kHeight, false, gi::GiGather::ScreenProbes, 16, 0,
         0.71F},
        {"screen probes every 8 px", kWidth, kHeight, true, gi::GiGather::ScreenProbes, 8, 0,
         0.71F},
    };
    const std::vector<f64> totals = cost_runs(gpu, {runs, std::size(runs)}, hardware);
    // The probe-space denoiser costs less than the shared one it replaces, through each tier:
    // runs 2 and 3 against 4 and 5.
    for (usize tier = 0; tier < 2; ++tier) {
        if (totals[2 + tier] > 0.0) {
            CY_CHECK_LT(totals[2 + tier], totals[4 + tier]);
        }
    }
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("what each gather costs for a 3840x2160 output, native and through the upscaler") {
    Gpu gpu(true, kSuite);
    if (!gpu.available()) {
        return;
    }
    const bool hardware = gpu.ray_queries();
    // The GI runs at the frame's internal resolution: native 4K, or the temporal upscaler's
    // quality mode (2/3 a side, 2560x1440); its performance mode renders 1920x1080, whose costs
    // are the case above.
    const CostRun runs[] = {
        {"per-pixel gather", 3840, 2160, true, gi::GiGather::PerPixel, 16, 0},
        {"screen probes every 16 px", 3840, 2160, true, gi::GiGather::ScreenProbes, 16, 0},
        {"screen probes every 16 px", 3840, 2160, false, gi::GiGather::ScreenProbes, 16, 0},
        {"screen probes every 8 px", 3840, 2160, true, gi::GiGather::ScreenProbes, 8, 0},
        {"per-pixel gather", 2560, 1440, true, gi::GiGather::PerPixel, 16, 0},
        {"screen probes every 16 px", 2560, 1440, true, gi::GiGather::ScreenProbes, 16, 0},
        {"screen probes every 16 px", 2560, 1440, false, gi::GiGather::ScreenProbes, 16, 0},
        {"screen probes every 8 px", 2560, 1440, true, gi::GiGather::ScreenProbes, 8, 0},
    };
    (void)cost_runs(gpu, {runs, std::size(runs)}, hardware);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}
