#pragma once
// The seam between this module's CPU visible sets and the GPU cull's buffers. M7 task 5.2.
//
// `rendering-culling-and-lod` — "GPU-driven culling", and its other half: "A CPU path SHALL remain
// for devices lacking the capability and for cases needing CPU visibility results (audio occlusion,
// gameplay queries)."
//
// ================================================================================================
// WHAT THIS FILE IS FOR
// ================================================================================================
//
// M6's closing gate recorded, of the module this file consumes: `cy::servers-render-culling` is
// linked by NOTHING but its own test binaries. That is the sentence task 5.2 exists to make false,
// and the way to make it false honestly is not a link line — it is for the renderer's own culling
// module to produce the dispatch's input and consume its output, so that a frame assembled from a
// GPU cull and a frame assembled from `cull_view()` are the same frame downstream.
//
// Three functions, one each way and one for the view:
//
//   `publish_gpu_cull_scene` turns the spatial index into the `GpuInstance` records a dispatch
//   reads. INDEXED BY GPU SCENE SLOT, because `gpu_cull.h` requires it: "Instance culling SHALL
//   read the GPU scene, so virtual geometry does not traverse ECS entities or maintain its own
//   instance list." The reverse map is published beside it, because a payload comes back naming a
//   GPU slot and a `VisibleInstance` is about a spatial one.
//
//   `write_gpu_cull_view` turns a `CullView` into a `GpuCullView`. One function, because the plane
//   order and the derived scalars are a contract with the shader and a second caller writing them
//   by hand would get it right until somebody reordered `Frustum::PlaneIndex`.
//
//   `apply_gpu_cull` turns the compacted payloads back into `CullResults` — the same structure
//   `cull_view()` fills and the same one `cy::rendering-forward` consumes. Nothing above this
//   module can tell which cull ran, which is the property that makes the GPU path adoptable rather
//   than a parallel renderer.
//
// ================================================================================================
// WHAT IS DELIBERATELY NOT HERE
// ================================================================================================
//
// The dispatch. It needs a device, a compute pipeline and a render graph, and this module names
// none of the three — which is what lets its whole suite run headless on a machine with no GPU.
// `cy::rendering-gpu-culling` owns the dispatch and depends on this module's vocabulary rather than
// the other way round.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/culling/cull.h>
#include <cy/rendering/culling/spatial.h>
#include <cy/servers/render/culling/gpu_cull.h>
#include <cy/servers/render/gpu_scene.h>

namespace cy::rendering {

/// The arrays a dispatch reads, published from the spatial index.
///
/// `instances` and `ranges` are indexed by GPU SCENE SLOT and `spatial_slots` is the reverse map —
/// `spatial_slots[gpu_slot]` is the spatial slot that published it, or `kNoSpatialSlot` for a GPU
/// slot nothing in this index published. Holes are not an error: a GPU scene is a slot allocator
/// and a renderer that published instances from two sources has two publishers filling one array.
struct GpuCullPublication {
    explicit GpuCullPublication(Allocator& allocator) noexcept;

    GpuCullPublication(const GpuCullPublication&) = delete;
    GpuCullPublication& operator=(const GpuCullPublication&) = delete;

    Array<render::GpuInstance> instances;
    Array<render::culling::GpuVisibilityRange> ranges;
    Array<u32> spatial_slots;
    /// One past the highest GPU slot published. What `GpuCullView::instance_count` must be set to.
    u32 high_water = 0;
    /// Whether any published instance declared a visibility range. False leaves `ranges` empty on
    /// the way to the dispatch, because an EMPTY span means "no instance declares one" and a
    /// populated span of zeroes means something else entirely.
    bool any_ranges = false;

    void clear() noexcept;
};

inline constexpr u32 kNoSpatialSlot = ~0U;

/// Publish the spatial index's renderables into the GPU scene layout.
///
/// Only `SpatialDomain::Renderable` entries are published: a light, a decal or a probe affects what
/// is drawn rather than being drawn, and an indirect draw for one would be a draw of nothing.
///
/// A NOTE ON WHAT THIS DOES NOT DO. It does not allocate GPU slots and it does not own the GPU
/// scene: `SpatialEntry::gpu_slot` is where the render server already put the instance, and this
/// function writes there. A renderer whose GPU scene is published by `render::GpuScene` should hand
/// that array to the dispatch directly and use this only for the reverse map — which is why the map
/// is a separate array rather than a field on the instance record.
[[nodiscard]] Status publish_gpu_cull_scene(const SpatialIndex& index,
                                            GpuCullPublication& out) noexcept;

/// Fill a `GpuCullView` from this module's own view description.
///
/// `instance_count` is the publication's high water mark rather than the index's slot count: the
/// dispatch covers GPU slots, and the two are different numbers whenever a slot has been freed.
void write_gpu_cull_view(const CullView& view, u32 instance_count,
                         render::culling::GpuCullView& out) noexcept;

/// Turn a dispatch's compacted output into the visible sets the frame consumes.
///
/// `results` is cleared first, exactly as `cull_view()` clears it, so a caller that reuses one
/// object across views cannot accumulate two views into one list.
///
/// The routing is the spatial flags', not the payload's: a transparent instance goes to
/// `transparent` and a moved one is additionally appended to `motion`, which is what `cull_view()`
/// does and what the sort downstream expects. The payload carries the LOD decision and the depth,
/// so `select_lods()` does NOT run afterwards — the dispatch already made that decision, and
/// running it again on the CPU would be the second implementation this whole arrangement exists to
/// avoid.
[[nodiscard]] Status apply_gpu_cull(const SpatialIndex& index, const GpuCullPublication& published,
                                    Span<const render::culling::GpuDrawPayload> payloads,
                                    const render::culling::GpuCullCounters& counters,
                                    CullResults& results) noexcept;

}  // namespace cy::rendering
