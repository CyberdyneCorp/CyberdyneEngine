#pragma once
// A scene's skinned instances, skinned in one submit from one device pose buffer. Issue #76
// stage 3.
//
// ================================================================================================
// WHAT `SkinPass` LEFT OPEN
// ================================================================================================
//
// `SkinPass` skins one mesh, driven by hand, from a bone buffer it fills itself. The module README
// said what was missing: "There is no per-instance skinning table and no dispatch that skins a
// scene's worth of characters in one submit", and `PoseWorld`'s header said whose job the device
// half of the pose world is: "NO DEVICE, NO BUFFER, NO UPLOAD ... the renderer owns the buffer it
// goes into and the frame it goes in". This is that renderer half:
//
//   ONE POSE BUFFER, the pose world on the device. `upload_poses` writes the range the world says
//   changed — `PoseWorld::upload_offset()` and `upload_size()` — and never the whole array. The
//   matrices land at the world's own indices, so `PoseWorld::matrix_offset(handle)` is the
//   `pose_offset` the dispatch reads with no translation.
//
//   ONE SKINNING TABLE. Meshes are added once (their bind pose, frames and influences, packed into
//   shared input buffers) and instances are added and removed without rebuilding anything: each
//   instance owns a window of the shared output buffers, twice its mesh's vertex count, because
//   the output is double buffered for motion vectors.
//
//   ONE PASS. `declare` adds one compute pass to the frame's graph that records a dispatch per
//   instance, each with its own push constants and all over one descriptor set. Every skinned
//   instance of a frame is skinned in that pass, so the depth prepass, the shadow map, the opaque
//   pass and the selection mask all read vertices the graph has ordered after it.
//
// The dispatch is `skin.slang`, unchanged: the output frame is `render::PackedNormalTangent`, the
// cooked encoding, and the frame binds it as `rhi::Format::Rgba16Snorm` through
// `FramePipelines::skinned_pipeline` — the gap `frame_pipelines.h` and `skin_dispatch.h` recorded
// is closed by the format, not by a second encoding.
//
// ================================================================================================
// WHAT IT DOES NOT DO
// ================================================================================================
//
// Blend shapes: the scene's dispatches name none. A mesh with active shapes is a `SkinPass`.
//
// Frames in flight: the same contract `SkinPass` states. The pose buffer is host-visible and the
// output is double buffered by frame PARITY, not by frame in flight, so a caller that submits a
// second frame before the first has completed must keep a scene per frame in flight. The suites
// drain each frame.
//
// No dependency on `cy::animation`: the renderer layer builds with `CY_ANIMATION=OFF`, so the pose
// arrives as the world's matrix span and dirty range rather than as a `PoseWorld`:
//
//     scene.upload_poses(world.matrices(), world.upload_offset(), world.upload_size());
//     world.clear_upload_range();
//     scene.set_pose(instance, world.matrix_offset(handle));

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/skinning/skin_pass.h>
#include <cy/servers/render/geometry/skin_dispatch.h>
#include <cy/servers/render/geometry/skinning.h>

namespace cy::rendering::skinning {

/// How large a scene the buffers are sized for. One allocation each, at creation.
struct SkinnedSceneDescription {
    /// Bind-pose vertices over every mesh added. An eight-influence mesh may cost up to its own
    /// vertex count again in alignment, because its influence records are two per vertex.
    u32 max_mesh_vertices = 0;
    /// Skinned vertices over every live instance. The output buffers hold twice this.
    u32 max_instance_vertices = 0;
    /// Matrices in the pose world: `PoseWorld::matrices().size()` at its largest.
    u32 max_pose_matrices = 0;
    u32 max_meshes = 0;
    u32 max_instances = 0;
    /// Host copies of the output and the transfer that fills them, for a suite that compares the
    /// dispatch against `cpu_reference_skin`. Off for a frame that only draws.
    bool read_back = false;
};

/// A mesh's bind pose, as the dispatch reads it.
struct SkinnedMeshDescription {
    Span<const Vec3> positions;
    Span<const render::PackedNormalTangent> frames;
    /// One record per vertex at four influences, two at eight.
    Span<const render::geometry::GpuSkinInfluence> influences;
    render::geometry::InfluenceCount influence_count = render::geometry::InfluenceCount::Four;
    /// The skeleton's bone count; the dispatch clamps every index against it.
    u32 bone_count = 0;
};

using SkinnedMeshId = u32;

/// One skinned instance. Generational: a handle kept past `remove_instance` is refused rather than
/// naming whichever instance took its slot.
struct SkinnedInstance {
    u32 index = 0xFFFFFFFFU;
    u32 generation = 0;

    [[nodiscard]] bool valid() const noexcept { return index != 0xFFFFFFFFU; }
};

/// Where an instance's skinned vertices are this frame, for a draw.
struct SkinnedOutput {
    rhi::BufferHandle positions;
    rhi::BufferHandle frames;
    /// The first vertex of the half this frame's dispatch wrote.
    u32 current_vertex = 0;
    /// The first vertex of the other half: last frame's positions, which motion vectors read.
    u32 previous_vertex = 0;
    /// False on the first frame an instance is skinned, when the other half holds nothing: a draw
    /// then reads the current positions as the previous ones and its motion is its placement's.
    bool has_previous = false;
    u32 vertex_count = 0;
};

/// What the last frame did.
struct SkinnedSceneStats {
    u32 meshes = 0;
    u32 instances = 0;
    /// Dispatches the last `declare` recorded — one per instance it skinned — and the graph passes
    /// it declared to do so (one, plus the read-back pair when it is on).
    u32 dispatches = 0;
    u32 passes = 0;
    /// Matrices the last `upload_poses` wrote, and over the scene's life.
    u32 uploaded_matrices = 0;
    u64 uploaded_matrices_total = 0;
};

class SkinnedScene {
public:
    explicit SkinnedScene(Allocator& allocator) noexcept;
    ~SkinnedScene();

    SkinnedScene(const SkinnedScene&) = delete;
    SkinnedScene& operator=(const SkinnedScene&) = delete;
    SkinnedScene(SkinnedScene&&) = delete;
    SkinnedScene& operator=(SkinnedScene&&) = delete;

    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    /// Create the pipeline and every buffer. Fails naming the field when a size is zero and the
    /// capability when the device cannot run the dispatch.
    [[nodiscard]] Status create(rhi::Device& device, const SkinnedSceneDescription& desc) noexcept;
    void destroy() noexcept;

    /// Add a mesh's bind pose to the shared input buffers. Once per mesh, not per instance.
    [[nodiscard]] Expected<SkinnedMeshId, Error> add_mesh(
        const SkinnedMeshDescription& mesh) noexcept;

    /// Give an instance of `mesh` a window of the output. Its pose is set per frame.
    [[nodiscard]] Expected<SkinnedInstance, Error> add_instance(
        SkinnedMeshId mesh, render::geometry::SkinningMethod method =
                                render::geometry::SkinningMethod::LinearBlend) noexcept;
    /// Release it. Its window is reused by the next instance of the same size; no other instance's
    /// window moves.
    [[nodiscard]] Status remove_instance(SkinnedInstance instance) noexcept;
    [[nodiscard]] bool live(SkinnedInstance instance) const noexcept;

    /// Write the pose world's changed range — `matrices[first, first + count)` — into the device
    /// pose buffer at the same indices. Nothing outside the range is touched. `count` zero writes
    /// nothing, which is a frame in which nothing was published.
    [[nodiscard]] Status upload_poses(Span<const Mat4> matrices, u32 first, u32 count) noexcept;

    /// Where `instance`'s bones begin in the pose buffer THIS FRAME —
    /// `PoseWorld::matrix_offset(handle)`, which moves at every publish. Required before the
    /// instance is skinned; an instance with no pose is left out of the dispatch.
    [[nodiscard]] Status set_pose(SkinnedInstance instance, u32 pose_offset) noexcept;

    /// Declare the pass that skins every instance with a pose, writing the half of each window
    /// `frame_index`'s parity selects. Nothing is declared when no instance has a pose, which is
    /// what leaves a frame with no skinned instance the frame it was.
    [[nodiscard]] Status declare(RenderGraph& graph, u64 frame_index) noexcept;

    /// The output resources the last `declare` created, for every pass that draws from them to
    /// declare its read — `FramePassCallback::vertex_reads`. Empty when nothing was declared.
    [[nodiscard]] Span<const ResourceId> vertex_reads() const noexcept;

    /// Where `instance` was skinned by the last `declare`. False for an instance that was not.
    [[nodiscard]] bool output(SkinnedInstance instance, SkinnedOutput& out) const noexcept;

    /// The constant block the last `declare` pushed for `instance` — what a test hands
    /// `cpu_reference_skin`.
    [[nodiscard]] Expected<render::geometry::GpuSkinConstants, Error> constants(
        SkinnedInstance instance) const noexcept;

    /// The output buffers, read back. Only on a scene created with `read_back`, after the frame's
    /// fence.
    [[nodiscard]] Expected<Span<const Vec3>, Error> read_back_positions() const noexcept;
    [[nodiscard]] Expected<Span<const render::PackedNormalTangent>, Error> read_back_frames()
        const noexcept;
    /// The device pose buffer as the dispatch reads it, for a test that checks what the upload did.
    [[nodiscard]] Span<const render::geometry::GpuBoneMatrix> pose_buffer() const noexcept;

    [[nodiscard]] const SkinnedSceneStats& stats() const noexcept { return stats_; }

private:
    struct Mesh {
        u32 first_vertex = 0;
        u32 vertex_count = 0;
        u32 bone_count = 0;
        render::geometry::InfluenceCount influences = render::geometry::InfluenceCount::Four;
    };

    struct Instance {
        SkinnedMeshId mesh = 0;
        u32 generation = 1;
        /// The first vertex of the instance's two-half window in the output buffers.
        u32 first_output = 0;
        u32 pose_offset = 0;
        render::geometry::SkinningMethod method = render::geometry::SkinningMethod::LinearBlend;
        /// The frame index the instance was last skinned in, for `has_previous`.
        u64 skinned_frame = 0;
        bool live = false;
        bool posed = false;
        bool skinned_once = false;
        /// Skinned by the last `declare`.
        bool skinned = false;
        /// The other half of the window holds the frame before the last `declare`'s.
        bool has_previous = false;
        render::geometry::GpuSkinConstants constants{};
    };

    struct Buffers {
        rhi::BufferHandle bones;
        rhi::BufferHandle bone_dual_quaternions;
        rhi::BufferHandle blend_shape_deltas;
        rhi::BufferHandle active_blend_shapes;
        rhi::BufferHandle in_positions;
        rhi::BufferHandle in_frames;
        rhi::BufferHandle influences;
        rhi::BufferHandle positions;
        rhi::BufferHandle frames;
        rhi::BufferHandle positions_readback;
        rhi::BufferHandle frames_readback;
    };

    [[nodiscard]] Status create_buffers() noexcept;
    [[nodiscard]] Expected<u32, Error> take_output_window(u32 vertices) noexcept;
    [[nodiscard]] const Instance* instance_of(SkinnedInstance instance) const noexcept;
    [[nodiscard]] Status prepare_dispatches(u64 frame_index) noexcept;
    static void record_skin(const PassContext& context, void* user) noexcept;
    static void record_readback(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    SkinnedSceneDescription desc_{};
    detail::SkinPipeline pipeline_;
    rhi::DescriptorSetHandle descriptor_set_;
    Buffers buffers_{};
    Array<Mesh> meshes_;
    Array<Instance> instances_;
    Array<u32> free_instances_;
    /// Freed output windows as (first vertex, vertices) pairs, reused by an instance of that size.
    Array<u64> free_windows_;
    /// The instances the last `declare` dispatched, in instance order.
    Array<u32> dispatched_;
    u32 next_vertex_ = 0;
    u32 next_record_ = 0;
    u32 next_output_ = 0;
    ResourceId reads_[2] = {kInvalidResource, kInvalidResource};
    u32 read_count_ = 0;
    SkinnedSceneStats stats_;
};

}  // namespace cy::rendering::skinning
