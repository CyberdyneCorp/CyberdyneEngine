#pragma once
// The GPU traversal path: the scene's buffers, the compute pipelines, and the passes that produce a
// visible cluster list without the CPU touching the hierarchy. M7 task 7.3.
//
// `virtual-geometry` — "GPU hierarchy traversal and cluster culling", and "No CPU draw loop": "WHEN
// hundreds of thousands of clusters are visible THEN the CPU SHALL submit a small, bounded number
// of passes with indirect arguments."
//
// The number here is bounded and small: one reset, one instance cull, and two dispatches per
// hierarchy level (a one-thread `prepare` that turns a count into dispatch arguments, and the
// traversal step dispatched from them). The CPU never learns how many nodes a level had.
//
// ================================================================================================
// WHY THIS IS DRIVEN THROUGH THE RENDER GRAPH AND NOT THROUGH THE DEVICE
// ================================================================================================
//
// Every one of those dispatches reads what the last one wrote, so every one of them needs a barrier
// — and `cy::rhi::CommandBuffer` deliberately has no way to emit one. `rendering-graph`'s executor
// is the only thing in the engine that can, and it derives them from declared accesses rather than
// from a pass author's opinion. So `GpuTraversal::record()` DECLARES its passes into a caller's
// `RenderGraph` rather than recording them into a command buffer, and the barriers between the
// levels are the graph's rather than this file's. That is the same rule `src/rendering/graph/`'s
// barrier passkey enforces in the type system, seen from the consuming side.
//
// ================================================================================================
// WHAT IS DELIBERATELY ABSENT AT M7, AND WHERE THE SEAM IS
// ================================================================================================
//
// **Occlusion culling.** `virtual-geometry` specifies a two-pass HZB scheme, and the renderer has
// no hierarchical depth buffer yet. `TraversalStatistics::nodes_pruned_by_occlusion` and
// `rejected_by_occlusion` exist, read zero, and are the seam: the shader's cull chain takes the
// tests in the order the requirement fixes and the HZB test slots in beside the cone test without
// changing anything above this file. Reported rather than left to be discovered.
//
// **A hash for the visited marks.** The DAG mark is one word per (instance, cluster), so a scene of
// many instances of a large asset needs more memory than it should. `create()` computes the
// allocation and REFUSES rather than overflowing, so the limit is a diagnostic instead of a
// corruption.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/virtual_geometry/residency.h>
#include <cy/rendering/virtual_geometry/traversal.h>

namespace cy::rendering::vg {

/// One asset's place in the concatenated scene buffers. `GpuInstance::asset` indexes an array of
/// these, so many assets share one cluster buffer and one page table.
struct GpuAsset {
    u32 cluster_base = 0;
    u32 cluster_count = 0;
    u32 root_first = 0;
    u32 root_count = 0;
    u32 page_base = 0;
    u32 page_count = 0;
    u32 child_base = 0;
    u32 reserved = 0;
};
static_assert(sizeof(GpuAsset) == 32, "GpuAsset must match cy/vg/vg_traversal.slang");

/// The scene the traversal runs over: every asset's clusters, roots and child table concatenated,
/// plus the instances that reference them. Built on the CPU once per asset set, not per frame.
struct GpuScene {
    explicit GpuScene(Allocator& allocator) noexcept;

    GpuScene(const GpuScene&) = delete;
    GpuScene& operator=(const GpuScene&) = delete;
    GpuScene(GpuScene&&) noexcept = default;
    GpuScene& operator=(GpuScene&&) noexcept = default;

    Array<GpuCluster> clusters;
    Array<u32> cluster_triangles;
    Array<GpuAsset> assets;
    Array<u32> roots;
    Array<u32> children;
    Array<GpuInstance> instances;

    /// Append one decoded asset. Returns its index, which is what a `GeometryInstance::asset`
    /// names.
    [[nodiscard]] Expected<u32, Error> add_asset(const DecodedAsset& asset) noexcept;
    [[nodiscard]] Status set_instances(Span<const GeometryInstance> source) noexcept;

    /// The largest cluster count of any asset — the stride of the `visited` marks.
    [[nodiscard]] u32 cluster_stride() const noexcept;
};

struct GpuTraversalOptions {
    /// Hierarchy levels the descent will iterate. Beyond it the queue is abandoned, which would
    /// leave a coarse cluster drawn rather than a hole; `TraversalReadback::levels_exhausted` says
    /// when it happened rather than leaving it to be noticed.
    u32 max_levels = 12;
    /// Entries in each traversal queue, and in the visible and request lists. Sized on the host so
    /// that an overflow is a refusal at creation rather than a silent truncation on the device.
    u32 queue_capacity = 1U << 16U;
    u32 visible_capacity = 1U << 16U;
    u32 request_capacity = 1U << 12U;
};

/// What one frame's traversal produced, read back after the frame completed.
struct TraversalReadback {
    explicit TraversalReadback(Allocator& allocator) noexcept;

    TraversalReadback(const TraversalReadback&) = delete;
    TraversalReadback& operator=(const TraversalReadback&) = delete;
    TraversalReadback(TraversalReadback&&) noexcept = default;
    TraversalReadback& operator=(TraversalReadback&&) noexcept = default;

    Array<VisibleCluster> visible;
    Array<PageRequest> requests;
    TraversalStatistics stats;
    /// True when the descent hit `max_levels` with nodes still queued.
    bool levels_exhausted = false;
    /// True when a list filled. The counts are still reported; the lists are truncated.
    bool overflowed = false;
};

/// The device-side traversal. One instance per view; the buffers are sized once and reused.
class GpuTraversal {
public:
    GpuTraversal(Allocator& allocator, rhi::Device& device) noexcept;
    ~GpuTraversal();

    GpuTraversal(const GpuTraversal&) = delete;
    GpuTraversal& operator=(const GpuTraversal&) = delete;
    GpuTraversal(GpuTraversal&&) = delete;
    GpuTraversal& operator=(GpuTraversal&&) = delete;

    /// Create the pipelines and the buffers, and upload the scene's static half.
    ///
    /// Fails naming the limit when the scene needs more marks, queue entries or visible slots than
    /// the options allow — at creation rather than on the device, where an overflow is silent.
    [[nodiscard]] Status initialise(const GpuScene& scene,
                                    const GpuTraversalOptions& options) noexcept;

    /// Upload the instance buffer. Called when instances move; the traversal does not need it every
    /// frame.
    [[nodiscard]] Status upload_instances(Span<const GpuInstance> instances) noexcept;
    /// Upload the page table. `virtual-geometry`: "Page table updates SHALL be applied on the GPU
    /// without a CPU round trip per page" — this is the batched write of the whole table, which is
    /// what `GeometryCache::table()` hands over.
    [[nodiscard]] Status upload_page_table(Span<const PageTableEntry> table) noexcept;

    /// Declare the traversal's passes into `graph`. The caller executes the graph; the barriers
    /// between the levels are the graph's, because nothing else in the engine may emit one.
    [[nodiscard]] Status record(RenderGraph& graph, const TraversalView& view,
                                u32 instance_count) noexcept;

    /// Read what the last recorded frame produced. Must be called after that frame has completed —
    /// the readback is a host-visible buffer and the caller owns the synchronisation, which is
    /// `wait_idle()` in a test and the frame fence in a renderer.
    [[nodiscard]] Status read_back(TraversalReadback& out) const noexcept;

    /// The buffer holding the visible cluster list, for a rasterisation pass that consumes it
    /// without a round trip, and the counter buffer whose visible slot an indirect draw reads.
    [[nodiscard]] rhi::BufferHandle visible_buffer() const noexcept { return visible_; }
    [[nodiscard]] rhi::BufferHandle counter_buffer() const noexcept { return counters_; }
    /// The scene buffers a later pass reads to turn a visible cluster into geometry. Exposed so
    /// that the visibility pass consumes the traversal's output ON THE DEVICE rather than through a
    /// readback, which is the whole point of producing it there.
    [[nodiscard]] rhi::BufferHandle cluster_buffer() const noexcept { return clusters_; }
    [[nodiscard]] rhi::BufferHandle instance_buffer() const noexcept { return instances_; }
    [[nodiscard]] rhi::BufferHandle asset_buffer() const noexcept { return assets_; }
    [[nodiscard]] u32 visible_capacity() const noexcept { return options_.visible_capacity; }
    [[nodiscard]] static constexpr u32 visible_count_offset() noexcept { return 2U * sizeof(u32); }

private:
    /// What one recorded dispatch needs at record time. A plain struct because `RecordFn` is a
    /// function pointer and a `void*`: the lifetime of the captured state is this object's, said
    /// explicitly rather than through a closure.
    struct PassState {
        GpuTraversal* self = nullptr;
        u32 pass = 0;    // 0 reset, 1 instance cull, 2 prepare, 3 traverse, 4 upload
        u32 parity = 0;  // which queue a traverse or prepare step reads
        u32 groups = 0;
        u64 args_offset = 0;
        /// For the upload pass: which slots were dirty AT RECORD TIME.
        ///
        /// It is captured here rather than read from `uploads_` when the pass records, and the
        /// distinction cost a debugging session: `record()` clears the dirty flags when it returns,
        /// and the pass's callback runs later, at execute time. Reading the flags then found every
        /// one of them false and copied nothing, which looked exactly like a shader that selected
        /// no clusters.
        u32 upload_mask = 0;
    };

    /// One buffer's region of the staging allocation, and whether it holds something the next
    /// recorded frame has to copy. See `stage()` for why an upload is a graph pass.
    struct Upload {
        u64 staging_offset = 0;
        u64 capacity = 0;
        u64 bytes = 0;
        bool dirty = false;
    };

    enum UploadSlot : u32 {
        kUploadClusters = 0,
        kUploadClusterTriangles,
        kUploadInstances,
        kUploadAssets,
        kUploadRoots,
        kUploadChildren,
        kUploadPageTable,
        kUploadCount,
    };

    [[nodiscard]] Expected<rhi::BufferHandle, Error> make_buffer(const char* name, u64 bytes,
                                                                 rhi::BufferUsage usage,
                                                                 rhi::MemoryUse memory) noexcept;
    [[nodiscard]] Status stage(u32 slot, const void* data, u64 bytes) noexcept;
    [[nodiscard]] Expected<rhi::ComputePipelineHandle, Error> make_pipeline(const char* name,
                                                                            Span<const u32> spirv,
                                                                            u32 parity) noexcept;
    static void record_pass(const PassContext& context, void* user) noexcept;
    void dispatch(const PassContext& context, const PassState& state) noexcept;

    rhi::Device& device_;
    GpuTraversalOptions options_;
    GpuView view_{};
    u32 cluster_stride_ = 0;
    bool initialised_ = false;

    rhi::BufferHandle clusters_;
    rhi::BufferHandle cluster_triangles_;
    rhi::BufferHandle instances_;
    rhi::BufferHandle assets_;
    rhi::BufferHandle roots_;
    rhi::BufferHandle children_;
    rhi::BufferHandle page_table_;
    rhi::BufferHandle queue_a_;
    rhi::BufferHandle queue_b_;
    rhi::BufferHandle counters_;
    rhi::BufferHandle dispatch_args_;
    rhi::BufferHandle visible_;
    rhi::BufferHandle requests_;
    rhi::BufferHandle visited_;
    rhi::BufferHandle staging_;
    rhi::BufferHandle readback_;

    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle pipeline_layout_;
    rhi::DescriptorSetHandle descriptors_;
    rhi::ShaderModuleHandle modules_[8];
    u32 module_count_ = 0;
    rhi::ComputePipelineHandle reset_;
    rhi::ComputePipelineHandle instance_cull_;
    rhi::ComputePipelineHandle prepare_[2];
    rhi::ComputePipelineHandle traverse_[2];

    Upload uploads_[kUploadCount];
    Array<PassState> states_;
};

}  // namespace cy::rendering::vg
