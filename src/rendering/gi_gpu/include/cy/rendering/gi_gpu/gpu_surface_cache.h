// SPDX-License-Identifier: MIT
#pragma once
// The surface cache's card lighting on the device. Issue #35, stage 2.
//
// ================================================================================================
// WHAT IT REPLACES, AND BEHIND WHICH SEAM
// ================================================================================================
//
// `gi::SurfaceCache::shade` — the one function in the host cache that is arithmetic rather than a
// decision. This class is a `gi::SurfaceShadingBackend`: install it with
// `IlluminationSystem::set_surface_shading` (or `SurfaceCache::set_shading_backend`) and the cache
// keeps allocating, invalidating, SELECTING and answering lookups exactly as before, while the
// pages it selects are shaded here. The selection is `SurfaceCache::select`, the host's own, so the
// device update is budgeted and prioritised by the function the host update is.
//
// Per selected page, in compute (gi_cards.slang):
//
//   direct       every light, shadowed through the shadow map for the directional light it was
//                captured for and through the uploaded distance field for every other light —
//                `gi::ShadowMapOccluder` over `SoftwareTracer::occluded`, transcribed;
//   indirect     `gi::CardGather`: cosine rays through the field, a hit read from LAST FRAME's card
//                radiance, a miss answered by the sky term — which is the multi-bounce feedback;
//   outgoing     emission + albedo * direct + accumulated, and the error the scheduler ranks on.
//
// ================================================================================================
// THE FRAME, ONE UPDATE AT A TIME
// ================================================================================================
//
//     cache.update(context);          // selects, and calls submit(): uploads what moved
//     shading.declare(graph);         // the shade, the commit and the copy back
//     // ... execute the graph, wait for its fence ...
//     cache.collect();                // or the next update(): retire() writes the results back
//
// A result for a page whose revision moved while it was in flight is dropped, so an invalidation
// between submit and retire is never undone by a stale answer.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/card_lighting.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/rendering/gi_gpu/gpu_scene.h>
#include <cy/rendering/graph/graph.h>

namespace cy::rendering::gi_gpu {

/// What the last `submit` handed the device.
struct GpuSurfaceSubmission {
    u64 frame = 0;
    /// Pages the shade dispatch covers: at most the budget, at most `max_selection`.
    u32 pages = 0;
    FieldUploadReport field{};
    CardUploadReport cards{};
};

class GpuSurfaceShading : public gi::SurfaceShadingBackend {
public:
    GpuSurfaceShading() noexcept = default;

    /// Bind the device scene and the host field it mirrors. `lookup_radius` is the surface cache's.
    void bind(GpuGiScene& scene, const gi::DistanceField& field, f32 lookup_radius) noexcept;

    /// The directional light's shadow map, or null. Uploaded at the next submit.
    void set_shadow_map(const gi::ShadowMap* map) noexcept { shadow_map_ = map; }
    void set_gather(const gi::CardGatherSettings& gather) noexcept;

    // --- gi::SurfaceShadingBackend ---------------------------------------------------------------

    [[nodiscard]] u32 submit(Span<const gi::SurfacePage> pages, Span<const u32> selected,
                             const gi::SurfaceUpdateContext& context) noexcept override;
    u32 retire(Span<gi::SurfacePage> pages) noexcept override;

    /// Declare the shade, the commit and the copy of the results to the host. Call once after each
    /// `submit` that accepted pages; a submission never declared is dropped at `retire`.
    [[nodiscard]] Status declare(RenderGraph& graph) noexcept;

    [[nodiscard]] const GpuSurfaceSubmission& last_submission() const noexcept {
        return submission_;
    }
    /// Why the last `submit` accepted nothing, when it did not.
    [[nodiscard]] const Error& last_error() const noexcept { return error_; }

private:
    GpuGiScene* scene_ = nullptr;
    const gi::DistanceField* field_ = nullptr;
    const gi::ShadowMap* shadow_map_ = nullptr;
    f32 lookup_radius_ = 0.5F;

    GpuSurfaceSubmission submission_{};
    /// The handle and revision of each submitted page, in selection order.
    Array<u32> handles_;
    Array<u32> revisions_;
    bool declared_ = false;
    Error error_{};

    GpuGiScene::Recording shade_{};
    GpuGiScene::Recording commit_{};
    GpuGiScene::Copy results_copy_{};
};

}  // namespace cy::rendering::gi_gpu
