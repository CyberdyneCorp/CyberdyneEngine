#pragma once
// Per-view visible sets: frustum culling, its typed result lists, and its diagnostics. Task 4.4.3.
//
// `rendering-culling-and-lod` — "Frustum culling": "Culling SHALL reject instances by, in order:
// layer mask against the view's mask, then conservative frustum-versus-AABB using precomputed plane
// sign masks, then optional per-instance distance limits", parallelised "above a configurable
// instance-count threshold, each worker producing a local visible list merged by pointer transfer".
//
// ================================================================================================
// THE ORDER OF THE THREE TESTS IS THE REQUIREMENT, NOT AN OPTIMISATION
// ================================================================================================
//
// "Layer rejection precedes geometry" is a scenario in its own right. A layer test is one AND over
// a word the loop has already loaded; the frustum test is six dot products. Running them the other
// way round would be correct and would cost six times as much on the instances a view does not
// draw, which in a scene with per-view layer masks is most of them.
//
// ================================================================================================
// WHAT "MERGED BY POINTER TRANSFER" MEANS HERE, AND WHAT IT DOES NOT
// ================================================================================================
//
// Each partition fills its own `Array<VisibleInstance>` — a 40-byte visibility record per survivor,
// never the instance's own data, which stays in the spatial index and is touched only for
// survivors. The merge then appends those arrays into the typed lists IN PARTITION ORDER.
//
// Appending rather than splicing is deliberate and it is the honest description: the typed lists
// are contiguous arrays a sort reads, and a list of blocks would push the concatenation into the
// sort instead. What the requirement is actually protecting against — copying per-instance payloads
// across a thread boundary — does not happen either way.
//
// PARTITION ORDER, NOT COMPLETION ORDER. `jobs::JobSystem::submit_parallel_for` partitions from the
// count and the grain alone, never from the worker count, so the merged list is byte-identical
// whatever the workers do. That is design.md §6 one level below the sort key: an order that
// depended on thread timing would make the sort's tie-break the only thing standing between the
// frame and non-determinism, and the sort should not be load-bearing for something the cull can
// simply not break.
//
// ================================================================================================
// WHAT IS DELIBERATELY ABSENT AT SEED
// ================================================================================================
//
// No occlusion culling and no GPU-driven culling. `rendering-culling-and-lod` specifies both;
// design.md §7 puts GPU-driven culling at M6, and the hierarchical-depth-buffer path belongs with
// it — the previous frame's depth, its reprojection and the two-pass scheme are a renderer-wide
// change rather than a function. What is here is the seam: `CullStatistics::rejected_by_occlusion`
// exists and reads zero, and `kSpatialIgnoreOcclusion` is already a flag, so the day the HZB lands
// nothing above this file changes shape.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/culling/lod.h>
#include <cy/rendering/culling/spatial.h>
#include <cy/servers/render/types.h>

namespace cy::jobs {
class JobSystem;
}  // namespace cy::jobs

namespace cy::rendering {

/// The view a cull is run for. Semantic rather than a matrix, for the same reason
/// `render::Projection` is: the frustum and the field of view are what the tests need, and a matrix
/// would have to be decomposed back into them.
struct CullView {
    Frustum frustum{};
    Vec3 camera_position{0.0F, 0.0F, 0.0F};
    /// Normalised. Distances are measured along it, so an instance at the edge of a wide frame is
    /// not judged further away than the same instance at the centre.
    Vec3 camera_forward{0.0F, 0.0F, -1.0F};
    render::LayerMask layer_mask = render::kAllLayers;
    f32 fov_y_radians = 1.0471975512F;
    /// Orthographic views size coverage from their height instead of their field of view.
    bool orthographic = false;
    f32 ortho_height = 10.0F;
    /// A view-wide draw distance. Zero means unlimited; a per-instance limit is applied as well and
    /// the smaller of the two wins.
    f32 max_distance = 0.0F;
    LodSettings lod;
};

/// One survivor. Small, because a frame produces tens of thousands and the merge's cost is memory
/// traffic — and because everything heavier is still in the spatial index, addressed by `slot`.
struct VisibleInstance {
    u32 slot = 0;
    u32 gpu_slot = 0;
    u64 stable_id = 0;
    /// Distance along the view direction. What the sort key quantises and what LOD selection used.
    f32 view_depth = 0.0F;
    f32 coverage = 0.0F;
    u32 lod_level = 0;
    u32 lod_fade_to = kInvalidLod;
    f32 lod_fade = 0.0F;
    f32 importance = 1.0F;
};

/// `rendering-culling-and-lod` — "Culling diagnostics": "instances tested, rejected by layer,
/// rejected by frustum, rejected by occlusion, rejected by visibility range, and finally visible;
/// plus LOD level histograms and culling wall-clock time".
struct CullStatistics {
    u32 tested = 0;
    u32 rejected_by_layer = 0;
    u32 rejected_by_frustum = 0;
    /// Always zero at Seed. See the header comment.
    u32 rejected_by_occlusion = 0;
    u32 rejected_by_range = 0;
    u32 visible = 0;
    /// How many survivors landed at each level. Index 7 accumulates every level past 6, which is
    /// deeper than any authored chain and is a bucket rather than a lost number.
    u32 lod_histogram[8] = {};
    u64 wall_nanoseconds = 0;
    /// How many partitions the parallel path used. One means it ran serially, which is what a
    /// count below the threshold gives and what a caller with no job system always gets.
    u32 partitions = 1;
};

/// The typed lists one view's cull produces.
///
/// FRAME-SCOPED. "WHEN the frame ends THEN all culling result memory SHALL be released by resetting
/// the frame arena": the allocator handed to the constructor is expected to be that arena, and
/// `clear()` is what a caller that reuses the object between frames calls. Neither is enforced here
/// — an arena is an allocator like any other — but the lists are sized for it.
struct CullResults {
    explicit CullResults(Allocator& allocator) noexcept;

    CullResults(const CullResults&) = delete;
    CullResults& operator=(const CullResults&) = delete;

    Array<VisibleInstance> opaque;
    Array<VisibleInstance> transparent;
    /// Instances that moved, and therefore need motion vectors. A subset of the two above rather
    /// than a partition of them: a moving transparent object is in both.
    Array<VisibleInstance> motion;
    Array<VisibleInstance> lights;
    Array<VisibleInstance> decals;
    Array<VisibleInstance> reflection_probes;
    Array<VisibleInstance> gi_volumes;
    CullStatistics stats{};

    void clear() noexcept;
};

/// How a cull is run, rather than what it looks at.
struct CullOptions {
    /// Below this many slots the cull runs on the calling thread. A parallel loop over a few
    /// hundred instances spends more in scheduling than it saves.
    u32 parallel_threshold = 4096;
    /// Slots per partition. Fixed rather than derived from the worker count, which is what makes
    /// the partitioning — and therefore the merged order — independent of the machine.
    u32 grain = 1024;
    /// Null runs serially whatever the threshold says.
    jobs::JobSystem* jobs = nullptr;
};

/// Per-partition scratch, so the parallel path allocates once per frame rather than once per view.
///
/// Held by the caller because a frame culls several views and they share it, and because an object
/// that allocated inside `cull_view` would allocate on the render thread every frame — which is the
/// thing a frame arena exists to prevent.
class CullWorkspace {
public:
    explicit CullWorkspace(Allocator& allocator) noexcept;
    ~CullWorkspace();

    CullWorkspace(const CullWorkspace&) = delete;
    CullWorkspace& operator=(const CullWorkspace&) = delete;

    /// Make room for `partitions` partition lists. Idempotent and cheap when the count has not
    /// changed, which is the common case across views of one size.
    [[nodiscard]] Status prepare(u32 partitions) noexcept;

    [[nodiscard]] u32 partition_count() const noexcept {
        return static_cast<u32>(partitions_.size());
    }
    [[nodiscard]] Array<VisibleInstance>& partition(u32 index) noexcept;
    /// The partition's own rejection counters, merged after the join. Held here rather than on the
    /// waiting thread's stack because the partitioner's ceiling is a thousand partitions and a
    /// thousand counter blocks is not a stack allocation.
    [[nodiscard]] CullStatistics& partition_statistics(u32 index) noexcept;

private:
    Allocator* allocator_ = nullptr;
    /// One list per partition. `Array` is move-only and holds its own allocator, so the outer array
    /// holds pointers rather than values: growing an `Array<Array<T>>` would move the inner arrays,
    /// which is legal but would invalidate a worker's reference mid-loop.
    Array<Array<VisibleInstance>*> partitions_;
    Array<CullStatistics> partition_stats_;
};

/// Cull one view. The three tests in the required order, then LOD selection for the survivors.
///
/// `results` is cleared first, so a caller that reuses one object across views cannot accidentally
/// accumulate two views into one list.
[[nodiscard]] Status cull_view(const SpatialIndex& index, const CullView& view,
                               const CullOptions& options, CullWorkspace& workspace,
                               CullResults& results) noexcept;

/// Where a survivor's LOD chain comes from. A callback rather than a table, because the chains live
/// in the render server's mesh records and this module may not hold a second copy of them — and
/// because a test supplies one chain for every instance in two lines.
using LodChainFn = Span<const render::MeshLod> (*)(u32 lod_chain, void* user);

/// Assign a level to every survivor of `results`, and fill the LOD histogram.
///
/// A SECOND PASS RATHER THAN PART OF THE CULL, deliberately. Selection reads the mesh table, which
/// a cull worker would need a second pointer to reach; and it runs over SURVIVORS, which are a
/// fraction of the instances tested. Running it inside the cull would put a table lookup on the
/// rejected instances' path, which is the path that has to stay cheap.
///
/// `previous_levels` is indexed by spatial slot and is read and written: it is where the hysteresis
/// band's state lives, and it belongs to the caller for the reason lod.h gives. An empty span
/// disables hysteresis, which is what a deterministic test wants.
[[nodiscard]] Status select_lods(const SpatialIndex& index, const CullView& view,
                                 LodChainFn chain_of, void* user, Span<u32> previous_levels,
                                 CullResults& results) noexcept;

// --- Shadow caster culling ---------------------------------------------------------------------

/// One shadow view's caster cull. `rendering-culling-and-lod` — "Shadow caster culling": a caster
/// list per shadow view (per cascade, per cube face), with casters "rejected when they cannot cast
/// into the camera frustum, by testing against the convex volume swept between the light and the
/// camera frustum".
struct ShadowCullView {
    /// The shadow projection's own frustum: the cascade's, or the cube face's.
    Frustum shadow_frustum{};
    /// The camera frustum the shadows will be seen in. Used only when `tight` is set.
    Frustum camera_frustum{};
    /// The light's direction of travel, normalised. For a point light, the face's axis.
    Vec3 light_direction{0.0F, -1.0F, 0.0F};
    /// How far a caster's bounds are swept along the light direction when testing whether it can
    /// reach the camera frustum. The cascade's own extent is the right number: further than that
    /// and the shadow falls outside the map anyway.
    f32 sweep_distance = 0.0F;
    render::LayerMask layer_mask = render::kAllLayers;
    /// FALSE DISABLES THE TIGHTER TEST, and that is a correctness switch rather than a quality one.
    /// "WHERE a light's shadow is rendered for multiple camera views in one frame, the tighter
    /// culling SHALL be disabled for that light so one shadow map is valid for all of them."
    bool tight = true;
};

struct ShadowCullStatistics {
    u32 tested = 0;
    u32 rejected_by_layer = 0;
    u32 rejected_by_frustum = 0;
    /// Rejected because, though inside the light's volume, they cannot cast into the camera
    /// frustum. The number that says whether the tighter test is earning its cost.
    u32 rejected_by_sweep = 0;
    u32 casters = 0;
};

/// Whether a caster inside the light's volume can cast into the camera's.
///
/// The swept volume is approximated by the union of the caster's box and the same box displaced
/// along the light direction — conservative, cheap, and exact enough that the only thing it can do
/// wrong is keep a caster that could have been dropped.
[[nodiscard]] bool casts_into_view(const Aabb& caster, const Frustum& camera_frustum,
                                   Vec3 light_direction, f32 sweep_distance) noexcept;

/// Produce one shadow view's caster list.
[[nodiscard]] Status cull_shadow_casters(const SpatialIndex& index, const ShadowCullView& view,
                                         Array<VisibleInstance>& casters,
                                         ShadowCullStatistics& stats) noexcept;

}  // namespace cy::rendering
