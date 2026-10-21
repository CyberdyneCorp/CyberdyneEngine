// SPDX-License-Identifier: MIT
#pragma once
// The device GI as one frame feature: what a host attaches to its assembled frame so the frame is
// lit by an `IlluminationSystem` it already runs, and so the GI's debug views are drawn by the
// device in that frame. Issue #35, the live half: the editor's viewport and a game's host.
//
// ================================================================================================
// WHAT A HOST DOES
// ================================================================================================
//
//   gi_gpu::GpuGiFrame gi;
//   gi.create(allocator, device, system, {width, height});   // once; again after `configure`
//   description.global_illumination = true;                   // or
//   assembly.set_global_illumination
//   // each frame, after the host's `system.update(context)`:
//   gi_gpu::GpuGiFrameTargets targets;
//   gi.prepare(graph, view, frame, targets);                   // syncs, begins, imports
//   assembly_view.screen_space_gi = targets.diffuse;
//   assembly_view.screen_space_gi_specular = targets.specular;
//   sinks.screen_space_gi = gi.stage();                       // the resolve, then the views
//   // once the frame's upload exists:
//   gi.set_jitter(jitter);
//   gi.write_control(diffuse_slot, specular_slot, upload.view.gi_control);
//
// Everything the host already decides stays the host's: the system's update (its lights, its
// camera, its budget), its settings, volumes, classes and emitter rules. The frame reads them
// through the caches the system keeps — the cards, the probes and the field — which `prepare`
// brings to the device every frame, so a setting the host changed between two frames is in the
// second frame's pixels. By default the caches' arithmetic runs on the device too — the card
// shading and the probe gather installed through the system's own seams, `set_surface_shading` and
// `set_probe_gather` — and the host's update is only the decisions: what to shade and gather, and
// where the probes go.
//
// ================================================================================================
// OFF IS THE FRAME IT WAS
// ================================================================================================
//
// A host that does not attach this has the frame it always had. A host that attaches it under a
// mode `exclusion_for()` admits no dynamic bounce in (`composites()` false) writes no words, and
// the frame is byte for byte the frame without it (`render.gi_gpu_frame` (a) and (b)); a host
// that wants no cost either does not attach it, which `FrameAssembly::set_global_illumination`
// lets it decide frame by frame.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/gi_gpu/gpu_budget.h>
#include <cy/rendering/gi_gpu/gpu_caches.h>
#include <cy/rendering/gi_gpu/gpu_debug_view.h>
#include <cy/rendering/gi_gpu/gpu_radiance_cache.h>
#include <cy/rendering/gi_gpu/gpu_resolve.h>
#include <cy/rendering/gi_gpu/gpu_scene.h>
#include <cy/rendering/gi_gpu/gpu_surface_cache.h>
#include <cy/rendering/graph/graph.h>

#include <memory>

namespace cy::rendering::rt_gpu {
class DeviceStructures;
}

namespace cy::rendering::gi_gpu {

struct GpuGiFrameDescription {
    /// The frame's extent.
    u32 width = 0;
    u32 height = 0;
    /// Trace through ray queries where the device has them and the system's acceleration service
    /// holds the scene's triangles; the field otherwise. The system's
    /// `GiRuntimeSettings::hardware_ray_tracing` still switches the hardware tier off at run time.
    bool hardware = true;
    /// Create the device debug views (`GpuGiDebugView`).
    bool debug_views = true;
    /// Shade the surface cache's cards and gather the radiance cache's probes on the device
    /// (`GpuSurfaceShading`, and the radiance cache as the system's `ProbeGatherBackend`): the
    /// system keeps every decision and the device does the arithmetic, so the host's update is the
    /// selection alone. Off, the caches are shaded and gathered by the host system, as the beauty
    /// shot's are — what a test that holds a view to the host's state frame by frame wants.
    bool device_caches = true;
    /// Host copies of the resolve's buffers and of every view drawn, for a test.
    bool readback = false;
    /// The device copies' capacities. `cache.max_probes` is replaced by every probe slot the
    /// system's clipmap can hold when `max_probes` is zero, the default.
    GpuGiSceneDescription scene{};
    GpuRadianceCacheDescription cache{};
    u32 max_probes = 0;
};

/// One frame's view, in the world the system's caches are in.
struct GpuGiFrameView {
    Vec3 eye{0.0F, 0.0F, 0.0F};
    /// World to clip, UNJITTERED.
    Mat4 world_to_clip = Mat4::identity();
    /// The projection the frame's prepass was drawn with, and its view rotation (camera-relative
    /// to view space): what the resolve unpacks the prepass through.
    Mat4 projection = Mat4::identity();
    Mat4 relative_to_view = Mat4::identity();
};

/// What `prepare` imported into the frame's graph.
struct GpuGiFrameTargets {
    /// `AssemblyView::screen_space_gi` and `screen_space_gi_specular`.
    ResourceId diffuse = kInvalidResource;
    ResourceId specular = kInvalidResource;
    /// The debug view's texture, which the opaque pass reads; invalid when no view is drawn.
    ResourceId debug_view = kInvalidResource;
};

class GpuGiFrame {
public:
    GpuGiFrame() noexcept;
    ~GpuGiFrame();

    GpuGiFrame(const GpuGiFrame&) = delete;
    GpuGiFrame& operator=(const GpuGiFrame&) = delete;
    GpuGiFrame(GpuGiFrame&&) = delete;
    GpuGiFrame& operator=(GpuGiFrame&&) = delete;

    /// Compute and a shader format the GI ships.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    /// Create the device half over `system`, which is borrowed, configured, and outlives this (or
    /// `destroy` comes first). With ray queries, the system's acceleration structures are mirrored
    /// and built on the device before this returns, in a device frame of its own: call it outside
    /// the host's `begin_frame`/`end_frame`.
    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                gi::IlluminationSystem& system,
                                const GpuGiFrameDescription& desc) noexcept;
    void destroy() noexcept;
    /// The system was destroyed before this: forget it, so `destroy` releases the device half
    /// without touching it (its acceleration service's mirror pointer died with it).
    void forget_system() noexcept {
        system_ = nullptr;
        caches_.forget_system();
    }
    [[nodiscard]] bool created() const noexcept { return device_ != nullptr; }
    [[nodiscard]] gi::IlluminationSystem* system() const noexcept { return system_; }

    // --- Each frame, after the host's `system.update` ------------------------------------------

    /// The view drawn over this frame, or `GiDebugView::Count` for none. Refused for a view the
    /// device does not draw for this system (`GpuGiDebugView::draws`).
    [[nodiscard]] Status set_debug_view(const GpuGiDebugViewSettings& settings) noexcept;
    /// Bring the device copies to the system's caches, start the frame, and import the targets
    /// into `graph`.
    [[nodiscard]] Status prepare(RenderGraph& graph, const GpuGiFrameView& view, u64 frame,
                                 GpuGiFrameTargets& out) noexcept;
    /// `FrameSinks::screen_space_gi`: the resolve's passes, then the debug view's.
    [[nodiscard]] FrameStageDeclaration stage() noexcept;
    /// The prepass's jitter for this frame, once the frame knows it.
    void set_jitter(Vec2 jitter) noexcept;
    /// Whether the system's mode admits the dynamic GI into any surface: false draws the frame
    /// as it was however much was traced.
    [[nodiscard]] bool composites() const noexcept;
    /// `FrameViewData::gi_control` for the two targets bound at those slots of the frame's texture
    /// table, under the system's mode.
    void write_control(u32 diffuse_slot, u32 specular_slot, u32 out[4]) const noexcept;

    // --- What it did ---------------------------------------------------------------------------

    [[nodiscard]] GpuResolveTimes last_times() const noexcept { return resolve_.last_times(); }
    /// The last frame's whole device cost: the resolve's stages, and — with `device_caches` — the
    /// card shading and the probe gather, timed with timestamps around their passes. What
    /// `GpuGiBudget` observes.
    [[nodiscard]] GpuGiCost last_cost() const noexcept;
    /// The resolve's measured device cost, for the host's next `FrameContext::measured_gi_ms`.
    [[nodiscard]] f32 measured_ms() const noexcept { return budget_.measured_ms(); }
    [[nodiscard]] gi::RadianceSource tier() const noexcept { return resolve_.last_tier(); }
    [[nodiscard]] bool hardware() const noexcept { return hardware_; }
    [[nodiscard]] GpuScreenResolve& resolve() noexcept { return resolve_; }
    [[nodiscard]] GpuGiDebugView& views() noexcept { return views_; }
    [[nodiscard]] rhi::TextureViewHandle diffuse_view() const noexcept {
        return resolve_.diffuse_view();
    }
    [[nodiscard]] rhi::TextureViewHandle specular_view() const noexcept {
        return resolve_.specular_view();
    }
    [[nodiscard]] rhi::TextureViewHandle debug_view() const noexcept {
        return views_.texture_view();
    }

private:
    [[nodiscard]] Status create_hardware() noexcept;
    static PassId declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    gi::IlluminationSystem* system_ = nullptr;
    GpuGiFrameDescription desc_{};
    GpuGiScene scene_;
    GpuRadianceCache cache_;
    /// The card shading and the probe gather on the device, with `device_caches`.
    GpuGiCaches caches_;
    GpuScreenResolve resolve_;
    GpuGiDebugView views_;
    GpuGiBudget budget_;
    std::unique_ptr<rt_gpu::DeviceStructures> structures_;
    GpuGiDebugViewSettings view_settings_{};
    bool hardware_ = false;
};

}  // namespace cy::rendering::gi_gpu
