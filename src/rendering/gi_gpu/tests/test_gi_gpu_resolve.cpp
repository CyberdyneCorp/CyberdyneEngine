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
#include "resolve_support.h"
#include "screen_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace cy;
namespace gi = cy::rendering::gi;
namespace rt = cy::rendering::rt;
using cy::rendering::apply_profile;
using cy::rendering::ArbiterReport;
using cy::rendering::BudgetArbiter;
using cy::rendering::BudgetSubsystem;
using cy::rendering::cost_source_name;
using cy::rendering::CostSource;
using cy::rendering::named_profile;
using cy::rendering::ProfileName;
using cy::rendering::QualityLadder;
using cy::rendering::gi_gpu::GpuResolveTimes;
using cy::rendering::gi_gpu::GpuScreenResolve;

namespace {

using namespace gi_gpu_support;  // NOLINT(google-build-using-namespace)

/// The room, the host, and the device beside it, run together for `frames` frames; every frame's
/// noisy, history and denoised buffers compared.
struct Comparison {
    /// The screen probes' records, every live probe of every frame, by its cell: coefficients.
    Agreement probe_coefficients;
    u32 probe_records = 0;
    /// Probes the two sides placed differently: present on one side only, or elsewhere.
    u32 probe_mismatches = 0;
    /// Adaptive candidates the host refused because a frame's capacity was spent.
    u32 refused = 0;
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

/// The device's probes against the host's, record by record: the uniform ones by tile, the
/// adaptive ones by their CELL, whichever slot each side gave them.
void compare_probes(const gi::ScreenProbeGather& host, const GpuScreenResolve& device,
                    Comparison& out) {
    const gi::ScreenProbeLayout& layout = host.layout();
    CY_REQUIRE(device.probe_layout().records() == layout.records());
    std::vector<gi::ScreenProbe> records(layout.records());
    std::vector<u32> slots(layout.cell_count());
    const Expected<u32, Error> adaptive =
        device.read_back_probes({records.data(), records.size()}, {slots.data(), slots.size()});
    CY_REQUIRE(adaptive.has_value());
    out.probe_mismatches += *adaptive == host.stats().adaptive ? 0U : 1U;
    out.refused += host.stats().refused;
    const auto compare = [&](const gi::ScreenProbe& mine, const gi::ScreenProbe& theirs) {
        if (mine.depth != theirs.depth || mine.id != theirs.id) {
            out.probe_mismatches += 1;
            return;
        }
        if (mine.depth <= 0.0F) {
            return;
        }
        out.probe_records += 1;
        // The count is the matched previous probe's, to its last bit.
        out.probe_mismatches +=
            std::abs(mine.history - theirs.history) <= 1.0e-5F * theirs.history ? 0U : 1U;
        // The estimate as a whole: the band-0 term is the probe's mean radiance, the scale.
        for (u32 k = 0; k < gi::kScreenProbeCoefficients; ++k) {
            out.probe_coefficients.add(mine.coefficients[k] + mine.coefficients[0],
                                       theirs.coefficients[k] + theirs.coefficients[0], 1.0e-3);
        }
    };
    for (u32 tile = 0; tile < layout.uniform_count(); ++tile) {
        compare(records[tile], host.probes()[tile]);
    }
    for (u32 cell = 0; cell < layout.cell_count(); ++cell) {
        const gi::ScreenProbe* theirs = host.adaptive(cell);
        const bool mine = slots[cell] != gi::kScreenProbeNone;
        if (mine != (theirs != nullptr)) {
            out.probe_mismatches += 1;
            continue;
        }
        if (theirs != nullptr) {
            compare(records[layout.uniform_count() + slots[cell]], *theirs);
        }
    }
}

[[nodiscard]] bool compare_room(Gpu& gpu, bool hardware, u32 frames, Comparison& out,
                                u32 divisor = 1, gi::GiGather gather = gi::GiGather::PerPixel,
                                u32 width = gi_support::kScreenWidth,
                                u32 height = gi_support::kScreenHeight, f32 rippled = 0.0F) {
    gi_support::ScreenScene host;
    host.settings.gather = gather;
    host.settings.probes.spacing_pixels = 8;
    // FOUR FRAMES OF PROBE HISTORY, not the default two: the blend's weight is the count's
    // (1/2, 1/3, 1/4) before it is the floor's, which a transcription could get wrong unseen.
    host.settings.probes.history_frames = 4;
    gi_support::make_room(host, width, height, hardware);
    if ((divisor != 1 && gather == gi::GiGather::PerPixel) || rippled > 0.0F) {
        // A RIPPLE OF A FEW DEGREES ON EVERY NORMAL, for the half grid. The room is flat, so
        // neighbouring normals agree exactly and the upsample's facing term — raised to its power
        // — would weigh nothing a transcription could get wrong; rippled, it weighs every tap.
        for (usize index = 0; index < host.view.normal.size(); ++index) {
            const u32 hash = gi::screen_hash(static_cast<u32>(index), 0, 11);
            const Vec3 ripple{gi::screen_unit(hash) - 0.5F,
                              gi::screen_unit(hash * 0x27220A95U) - 0.5F,
                              gi::screen_unit(hash * 0x9E3779B9U) - 0.5F};
            host.view.normal[index] =
                normalize(host.view.normal[index] + (ripple * (rippled > 0.0F ? rippled : 0.3F)));
        }
    }
    if (divisor != 1) {
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
        if (gather == gi::GiGather::ScreenProbes) {
            compare_probes(host.resolve.probes(), device.resolve, out);
        }
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
    out.probe_coefficients.finish();
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

/// The screen probes' records beside `check`'s signals. A probe's placement compares a weight
/// against a threshold, so a weight within a last bit of it may place differently on the two
/// sides; the bound is on how many do.
void check_probes(const Comparison& result, const char* tier) {
    std::fprintf(stderr, "%s, the probes: %u live records compared, %u placed differently\n", tier,
                 result.probe_records, result.probe_mismatches);
    result.probe_coefficients.print("  probe coefficients, every frame");
    CY_CHECK_GT(result.probe_records, 0U);
    CY_CHECK_LE(result.probe_mismatches * 100U, std::max(result.probe_records, 100U));
    CY_CHECK_LT(result.probe_coefficients.mean, 1.0e-3);
    CY_CHECK_LT(result.probe_coefficients.fraction_beyond(), 0.02);
    check(result, tier);
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

CY_TEST_CASE(
    "the device screen probes, their integration and the denoiser are the host's, software") {
    Gpu gpu(false);
    if (!gpu.available()) {
        return;
    }
    // Twelve frames: past the probes' history cap, and through the guide drawn from the history.
    Comparison result;
    CY_REQUIRE(compare_room(gpu, false, 12, result, 1, gi::GiGather::ScreenProbes, 64, 40));
    CY_CHECK_EQ(result.tier, gi::RadianceSource::SoftwareTrace);
    check_probes(result, "software screen probes");
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("on a half-resolution grid the device screen probes are the host's") {
    Gpu gpu(false);
    if (!gpu.available()) {
        return;
    }
    // NO RIPPLE ON THE NORMALS, as the per-pixel half grid has. A rippled normal turns a probe's
    // rays by its last bit on one side and not the other, a ray that grazes an edge then meets the
    // other side of it, and the probe's history carries that one ray's answer for frames and into
    // its neighbours' reprojection: two valid estimates, not one transcription. The upsample the
    // ripple exercises is the per-pixel half grid's case; this one holds the probes' placement,
    // reprojection and integration on the half grid.
    Comparison result;
    CY_REQUIRE(compare_room(gpu, false, 8, result, 2, gi::GiGather::ScreenProbes, 64, 40));
    check_probes(result, "software screen probes, divisor 2");
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

/// The rippled-placement case's ripple: up to about five degrees on every normal.
constexpr f32 kRipple = 0.18F;

CY_TEST_CASE("on rippled normals the device places the probes the host places") {
    Gpu gpu(false);
    if (!gpu.available()) {
        return;
    }
    // THE PLACEMENT ALONE, on normals rippled by a few degrees, so every normal weight is
    // fractional and a placement that weighed normals differently places differently. The probes'
    // estimates are not compared here: a rippled normal turns a ray by its last bit, and a ray
    // that grazes an edge then meets its other side (see the half-grid case). A larger ripple
    // asks for more adaptive probes than a frame keeps, and which of them the device's atomic
    // keeps is the order its threads arrive in.
    Comparison result;
    CY_REQUIRE(compare_room(gpu, false, 2, result, 1, gi::GiGather::ScreenProbes, 64, 40, kRipple));
    std::fprintf(stderr,
                 "rippled normals: %u live records compared, %u placed differently, %u adaptive "
                 "refused\n",
                 result.probe_records, result.probe_mismatches, result.refused);
    CY_CHECK_EQ(result.refused, 0U);
    CY_CHECK_GT(result.probe_records, 0U);
    CY_CHECK_EQ(result.probe_mismatches, 0U);
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the device screen probes are the host's, hardware tier with ray queries") {
    Gpu gpu(true);
    if (!gpu.available() || !gpu.ray_queries()) {
        return;
    }
    Comparison result;
    CY_REQUIRE(compare_room(gpu, true, 8, result, 1, gi::GiGather::ScreenProbes, 64, 40));
    CY_CHECK_EQ(result.tier, gi::RadianceSource::HardwareTrace);
    check_probes(result, "hardware screen probes");
    CY_CHECK_EQ(gpu.fixture.validation_errors(), 0U);
}

CY_TEST_CASE("what the per-pixel gather costs at 1920x1080, per stage, per tier and per lever") {
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
        // The per-pixel gather's costs: the screen probes' are smoke.gi_gpu_probe_quality's.
        host.settings.gather = gi::GiGather::PerPixel;
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
                     "1920x1080, per-pixel gather, %s: reproject %.3f, resolve %.3f, accumulate "
                     "%.3f, variance %.3f, filter %.3f, publish %.3f ms; total %.3f ms\n",
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
