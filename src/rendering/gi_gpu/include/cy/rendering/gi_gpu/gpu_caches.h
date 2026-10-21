// SPDX-License-Identifier: MIT
#pragma once
// The GI caches' per-frame arithmetic on the device: the surface cache's card shading (stage 2)
// and the radiance cache's probe gather (stage 3), installed behind the system's own seams and run
// in the frame before anything reads the caches. Issue #35: what a game's frame runs every frame.
//
// ================================================================================================
// WHAT STAYS ON THE HOST, AND WHAT IT COSTS
// ================================================================================================
//
// `IlluminationSystem::update` keeps every DECISION: which cards and probes this frame updates —
// the budgeted, prioritised selection both caches make, in time linear in the cache — where the
// probes go as the camera scrolls, what an invalidation touched. Installed, this class takes the
// ARITHMETIC: the shade of every selected card (its footprint's shadow tests, its card-to-card
// gather) and the trace of every selected probe, on the device, with only what changed uploaded
// (cards by revision, field bricks by what moved) and only the selection's results read back for
// the next update's schedule. The host path stays the oracle: uninstalled (`destroy`), the same
// system shades and gathers on the processor, and `render.gi_gpu` holds the two to each other.
//
//   gi_gpu::GpuGiCaches caches;
//   caches.create(device, scene, radiance, system);      // installs both seams
//   // each frame, after system.update(context):
//   caches.declare(graph);                               // before the passes that read the caches
//   // after the frame completed:
//   const gi_gpu::GpuGiCost cost = caches.cost();        // the cards' and the gather's device time
//
// `GpuGiFrame` owns one; a host that drives `GpuScreenResolve` itself (the beauty sample) owns one
// beside it.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/gi_gpu/gpu_budget.h>
#include <cy/rendering/gi_gpu/gpu_radiance_cache.h>
#include <cy/rendering/gi_gpu/gpu_scene.h>
#include <cy/rendering/gi_gpu/gpu_surface_cache.h>
#include <cy/rendering/graph/graph.h>

namespace cy::rendering::gi_gpu {

class GpuGiCaches {
public:
    GpuGiCaches() noexcept = default;
    ~GpuGiCaches();

    GpuGiCaches(const GpuGiCaches&) = delete;
    GpuGiCaches& operator=(const GpuGiCaches&) = delete;
    GpuGiCaches(GpuGiCaches&&) = delete;
    GpuGiCaches& operator=(GpuGiCaches&&) = delete;

    /// Install the device card shading over `scene` and `radiance` as `system`'s probe gather. All
    /// four are borrowed and outlive this (or `destroy` comes first). The radiance cache must not
    /// upload host-shaded cards (`GpuRadianceCache::bind_host_surfaces(nullptr)`): the cards'
    /// radiance is the device's own. With timestamp queries, the two stages are timed.
    [[nodiscard]] Status create(rhi::Device& device, GpuGiScene& scene, GpuRadianceCache& radiance,
                                gi::IlluminationSystem& system) noexcept;
    /// The host shades and gathers again from the next update.
    void destroy() noexcept;
    /// The system was destroyed first: release the device half without touching it.
    void forget_system() noexcept { system_ = nullptr; }
    [[nodiscard]] bool created() const noexcept { return device_ != nullptr; }

    /// What the system's last update submitted, run in this frame: each submission is declared
    /// once, in the frame after its update. A frame whose update submitted nothing declares
    /// nothing.
    [[nodiscard]] Status declare(RenderGraph& graph) noexcept;

    /// The last frame's card shading and probe gather, timed on the device. `resolve_*` is left
    /// as it is in `base`.
    [[nodiscard]] GpuGiCost cost(GpuGiCost base = {}) const noexcept;

    [[nodiscard]] GpuSurfaceShading& shading() noexcept { return shading_; }

private:
    void declare_stamp(RenderGraph& graph, u32 index) noexcept;
    static void record_stamp(const PassContext& context, void* user) noexcept;

    rhi::Device* device_ = nullptr;
    gi::IlluminationSystem* system_ = nullptr;
    GpuRadianceCache* radiance_ = nullptr;
    GpuSurfaceShading shading_;
    u64 shading_declared_ = ~u64{0};
    u64 gather_declared_ = ~u64{0};
    /// Timestamps before the card shading, between it and the gather, and after the gather.
    rhi::QueryPoolHandle timer_;
    bool timed_shading_ = false;
    bool timed_gather_ = false;
    bool timer_written_ = false;
    struct Stamp {
        GpuGiCaches* self = nullptr;
        u32 index = 0;
    };
    Stamp stamps_[3]{};
};

}  // namespace cy::rendering::gi_gpu
