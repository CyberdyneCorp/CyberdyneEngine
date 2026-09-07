#pragma once
// Instances, the reference traversal, and the GPU record layouts the shaders share with it.
// M7 task 7.3.
//
// `virtual-geometry` — "GPU hierarchy traversal and cluster culling": "For surviving instances,
// hierarchy traversal and cluster selection SHALL run **entirely on the GPU** ... The CPU SHALL NOT
// traverse the hierarchy, and SHALL NOT issue per-cluster or per-instance draw calls."
//
// ================================================================================================
// SO WHY IS THERE A CPU TRAVERSAL IN THIS HEADER AT ALL
// ================================================================================================
//
// Because a compute shader that culls is a compute shader nothing can check. M6 built
// `cpu_reference_cull` for exactly this reason and M7 task 5.1 spends it — "the compute dispatch,
// checked against `cpu_reference_cull` by comparing buffers" — and the same argument applies one
// level down. `traverse_reference()` is not the shipping path and is never called by a frame: it is
// the answer the dispatch is compared against, buffer for buffer, in `test_gpu_traversal.cpp`.
//
// The two implementations are kept in agreement by construction where it is possible: the selection
// test itself is `cluster_selected()` in cluster.h, called by the reference and transcribed once
// into `cy/vg/select.slang`, and the suite asserts the transcription by comparing outputs over a
// sweep of thresholds rather than at one.
//
// ================================================================================================
// THE RECORDS ARE THE WIRE FORMAT
// ================================================================================================
//
// Every `Gpu*` struct here is laid out to match a Slang structured buffer element exactly: scalars
// grouped so that no member straddles a 16-byte boundary, no `bool`, no enum, and a static
// assertion on the size of each. A mismatch here is not a compile error on either side — it is a
// shader reading the wrong field, which looks like a culling bug.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/virtual_geometry/asset.h>
#include <cy/rendering/virtual_geometry/cluster.h>

namespace cy::rendering::vg {

/// One instance of one asset. `virtual-geometry` — "Instancing and assemblies": "Instances of one
/// asset SHALL share its hierarchy and pages entirely, differing only in transform and material
/// parameters".
///
/// UNIFORM SCALE ONLY, and it is a decision rather than an omission: a geometric error is a
/// distance, and a non-uniform scale makes it a different distance along each axis. Supporting one
/// would mean either a conservative bound (the largest axis, which coarsens the whole object) or
/// an error per axis, which the selection test has no room for. An asset that needs a squashed
/// instance is cooked squashed.
struct GeometryInstance {
    Vec3 translation{0.0F, 0.0F, 0.0F};
    f32 scale = 1.0F;
    Quat rotation;
    /// Which asset's cluster table this instance draws from. Index into the scene's asset list.
    u32 asset = 0;
    /// Added to each cluster's material index, so one asset's instances can use different material
    /// tables without recooking.
    u32 material_offset = 0;
    /// `virtual-geometry` — "Gameplay API": "enable, quality bias, and importance" is the whole of
    /// what gameplay may tune. This is the bias; `importance` below is the other.
    f32 quality_bias = 1.0F;
    u32 layer_mask = 0xFFFFFFFFU;
    Importance importance = Importance::Normal;

    /// The instance's effective threshold scale: its importance and its bias together.
    [[nodiscard]] f32 threshold_scale() const noexcept {
        return importance_threshold_scale(importance) * quality_bias;
    }
};

/// One view a traversal runs for. `virtual-geometry` — "The threshold SHALL be settable per view,
/// and each view SHALL draw from its own allocation, so secondary views cost less and degrade
/// before the primary view does."
struct TraversalView {
    Frustum frustum{};
    ProjectionView projection;
    /// `virtual-geometry` — "Quality SHALL be expressed as a **maximum acceptable geometric error
    /// in pixels**, with named presets". This is that number, before the instance's own scaling.
    f32 threshold_pixels = 1.0F;
    /// `virtual-geometry` — "Shadows use coarser geometry": "the threshold SHALL be scaled so
    /// coarser clusters are selected than for the main view".
    f32 secondary_scale = 1.0F;
    /// Instances whose projected screen size in pixels is below this are dropped before any cluster
    /// work. Zero disables the test.
    f32 minimum_instance_pixels = 2.0F;
    u32 layer_mask = 0xFFFFFFFFU;
    /// Reject clusters whose normal cone faces entirely away from the camera.
    bool cone_culling = true;
};

/// One surviving cluster, as traversal emits it. The rasteriser's input and the visibility buffer's
/// identity in one record.
struct VisibleCluster {
    u32 instance = 0;
    u32 cluster = 0;
    u32 material = 0;
    /// Pixels. The achieved screen error, which the per-object inspector reports and the diagnostic
    /// answers "why was this cluster rendered" with.
    f32 screen_error = 0.0F;
};

/// One page traversal wanted and could not have. `virtual-geometry` — "GPU-driven streaming
/// feedback": "During traversal, encountering a non-resident page SHALL append a request to a GPU
/// buffer."
struct PageRequest {
    u32 asset = 0;
    u32 page = 0;
    /// How badly. Larger is more urgent: the projected screen error of the cluster that wanted it,
    /// scaled by the instance's importance. Requests are prioritised on this when more are made
    /// than the streaming budget allows.
    f32 priority = 0.0F;
    u32 requests = 1;
};

/// `virtual-geometry` — "Visualisation and diagnostics": "The profiler SHALL report per frame:
/// instance counts before and after culling, candidate and visible cluster counts, visible triangle
/// count, geometry cache occupancy and budget, streaming throughput, missing page count".
struct TraversalStatistics {
    u32 instances_tested = 0;
    u32 instances_visible = 0;
    u32 instances_rejected_by_layer = 0;
    u32 instances_rejected_by_frustum = 0;
    u32 instances_rejected_by_size = 0;
    u32 nodes_visited = 0;
    u32 nodes_pruned_by_frustum = 0;
    u32 nodes_pruned_by_occlusion = 0;
    u32 candidates = 0;
    u32 rejected_by_cone = 0;
    u32 rejected_by_frustum = 0;
    u32 rejected_by_size = 0;
    u32 rejected_by_occlusion = 0;
    u32 visible_clusters = 0;
    u32 visible_triangles = 0;
    /// Clusters rendered at a coarser level than the threshold asked for, because the page holding
    /// the finer one was not resident. The resident-root fallback, counted.
    u32 fallback_clusters = 0;
    u32 missing_pages = 0;
};

/// What a traversal produced.
struct TraversalResult {
    explicit TraversalResult(Allocator& allocator) noexcept;

    TraversalResult(const TraversalResult&) = delete;
    TraversalResult& operator=(const TraversalResult&) = delete;
    TraversalResult(TraversalResult&&) noexcept = default;
    TraversalResult& operator=(TraversalResult&&) noexcept = default;

    Array<VisibleCluster> visible;
    Array<PageRequest> requests;
    TraversalStatistics stats;

    void clear() noexcept;
};

/// Which pages a traversal may read. `virtual-geometry` — "When a required page is not resident,
/// rendering SHALL use the **nearest resident ancestor** rather than omitting the object", and "An
/// object SHALL never fail to render because streaming has not completed."
///
/// A function pointer rather than an interface: the residency answer is one bit per page and the
/// call is in the innermost loop of the reference traversal, which the GPU path answers from a
/// buffer.
using PageResidentFn = bool (*)(u32 asset, u32 page, void* user);

/// Every page resident. What a test that is not about streaming passes.
[[nodiscard]] bool all_pages_resident(u32 asset, u32 page, void* user) noexcept;

struct TraversalInputs {
    /// One decoded asset per `GeometryInstance::asset`.
    Span<const DecodedAsset* const> assets;
    Span<const GeometryInstance> instances;
    PageResidentFn resident = &all_pages_resident;
    void* resident_user = nullptr;
};

/// The reference traversal: instance culling, hierarchy descent with pruning, cluster culling, and
/// the resident-ancestor fallback. Deterministic and order-defined — instances in order, and within
/// an instance the clusters in the order the descent reaches them — so that a comparison against
/// the GPU path is a comparison of sets AND of a canonical ordering, which is what makes a
/// mismatch nameable.
[[nodiscard]] Status traverse_reference(const TraversalInputs& inputs, const TraversalView& view,
                                        TraversalResult& out) noexcept;

// ================================================================================================
// THE GPU RECORDS
// ================================================================================================

/// One cluster, as the traversal shaders read it. 96 bytes: everything the two tests and the two
/// culls need, and nothing they do not — the bounds and cone for culling, the two error/sphere
/// pairs for selection, the child range for descent, and the page for the residency test.
struct GpuCluster {
    f32 bounds_min[3];
    f32 lod_error;
    f32 bounds_max[3];
    f32 parent_error;
    f32 lod_center[3];
    f32 lod_radius;
    f32 parent_center[3];
    f32 parent_radius;
    f32 cone_axis[3];
    f32 cone_cos;
    u32 material;
    u32 page;
    u32 first_child;
    u32 child_count;
};
static_assert(sizeof(GpuCluster) == 96, "GpuCluster must match cy/vg/records.slang");

/// One instance, as the shaders read it. 48 bytes.
struct GpuInstance {
    f32 translation[3];
    f32 scale;
    f32 rotation[4];
    u32 asset;
    u32 material_offset;
    f32 threshold_scale;
    u32 layer_mask;
};
static_assert(sizeof(GpuInstance) == 48, "GpuInstance must match cy/vg/records.slang");

/// The per-view constants, as a push constant block. Kept under 128 bytes, which
/// `rhi-and-render-graph`'s `kMaxPushConstantBytes` is.
struct GpuView {
    f32 frustum[6][4];
    f32 camera[3];
    f32 pixels_scale;  // viewport_height / (2 * tan(fov/2))
    f32 threshold;
    u32 instance_count;
    u32 cluster_count;
    u32 flags;  // bit 0: cone culling
};
static_assert(sizeof(GpuView) == 128, "GpuView must fit kMaxPushConstantBytes");

/// One entry of the traversal queue, and of the candidate list.
///
/// `ancestor` is the deepest cluster on the path to this node whose page is resident, and it is
/// carried down rather than looked up: a cluster record has a CHILD range and no parent pointer, so
/// "the nearest resident ancestor" is a fact about the descent and not a fact about the cluster. It
/// is what the traversal substitutes when a selected cluster's page has not arrived, which is the
/// requirement's "SHALL use the nearest resident ancestor rather than omitting the object".
///
/// Sixteen bytes rather than twelve: a three-scalar element's stride depends on the layout rule the
/// shader compiler applies to a structured buffer, and a padded fourth field costs a megabyte per
/// 65,536 queue entries and removes the question.
struct GpuNode {
    u32 instance;
    u32 cluster;
    u32 ancestor;
    u32 reserved;
};
static_assert(sizeof(GpuNode) == 16, "GpuNode must match cy/vg/records.slang");

/// One visible cluster on the GPU, matching `VisibleCluster` field for field so the readback is a
/// memcpy and the comparison against the reference is exact.
struct GpuVisibleCluster {
    u32 instance;
    u32 cluster;
    u32 material;
    f32 screen_error;
};
static_assert(sizeof(GpuVisibleCluster) == 16, "GpuVisibleCluster must match VisibleCluster");

/// Pack a decoded asset's clusters into the shader's record layout.
[[nodiscard]] Status pack_clusters(const DecodedAsset& asset, Array<GpuCluster>& out) noexcept;

/// Pack instances into the shader's record layout.
[[nodiscard]] Status pack_instances(Span<const GeometryInstance> instances,
                                    Array<GpuInstance>& out) noexcept;

/// Fill the per-view constants from a traversal view.
[[nodiscard]] GpuView pack_view(const TraversalView& view, u32 instance_count,
                                u32 cluster_count) noexcept;

}  // namespace cy::rendering::vg
