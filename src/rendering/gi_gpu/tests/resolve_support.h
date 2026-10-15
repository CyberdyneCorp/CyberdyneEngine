// SPDX-License-Identifier: MIT
#pragma once
// The device half of the per-pixel and screen-probe resolve beside a host `ScreenScene`, shared by
// render.gi_gpu_resolve (the device against its oracle) and smoke.gi_gpu_probe_quality (what the
// gathers are worth at 1080p and 4K). Issue #35, stage 4 and its follow-up.

#include <cy/backends/rhi/device.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/screen_resolve.h>
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
#include <cstdio>
#include <vector>

namespace gi_gpu_support {

using namespace cy;  // NOLINT(google-build-using-namespace)
namespace gi = cy::rendering::gi;
namespace rt = cy::rendering::rt;
namespace rt_gpu = cy::rendering::rt_gpu;
using cy::rendering::gi_gpu::GpuGiScene;
using cy::rendering::gi_gpu::GpuGiSceneDescription;
using cy::rendering::gi_gpu::GpuRadianceCache;
using cy::rendering::gi_gpu::GpuRadianceCacheDescription;
using cy::rendering::gi_gpu::GpuResolveTimes;
using cy::rendering::gi_gpu::GpuResolveView;
using cy::rendering::gi_gpu::GpuScreenResolve;
using cy::rendering::gi_gpu::GpuScreenResolveDescription;

inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

/// A device, or a loud skip. The message is the one the suite's SKIP_REGULAR_EXPRESSION matches.
struct Gpu {
    render_test::DeviceFixture fixture;

    explicit Gpu(bool ray_tracing = true, const char* suite = "render.gi_gpu_resolve")
        : fixture("vulkan", suite, true, ray_tracing) {}

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
[[nodiscard]] inline bool run_frame(rhi::Device& device, Declare&& declare) {
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

[[nodiscard]] inline GpuGiSceneDescription scene_description() noexcept {
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
    /// `invalidated` is what the host filed this frame; `same_view` keeps last frame's surfaces
    /// rather than writing them again, for a view that does not move.
    [[nodiscard]] bool frame(rhi::Device& device, gi_support::ScreenScene& host, u64 frame,
                             Span<const Aabb> invalidated = {}, bool same_view = false) {
        if (!resolve.sync(host.system.radiance()).has_value()) {
            return false;
        }
        resolve.set_cache_frame(frame);
        GpuResolveView view;
        view.camera = host.view.camera;
        view.world_to_clip = host.view.world_to_clip;
        if (!resolve.begin_frame(view, frame).has_value()) {
            return false;
        }
        resolve.set_invalidated(invalidated);
        const Status surfaces =
            same_view ? resolve.keep_surfaces() : resolve.set_surfaces(host.view.surfaces());
        if (!surfaces) {
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

}  // namespace gi_gpu_support
