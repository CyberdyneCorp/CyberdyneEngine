#pragma once
// The skinning compute pass: the dispatch `cpu_reference_skin` was written to be checked against,
// and the first thing in this engine that moves a vertex. M8.d.
//
// `rendering-geometry-and-resources` — "Skinning": "Skinned meshes SHALL be transformed by a
// **compute pass** writing into per-instance output vertex buffers, so the result is reusable
// across passes (depth, shadows, main) without re-skinning", and "Bone matrices SHALL be read from
// the **GPU pose world** (see `animation-and-skinning`)".
//
// ================================================================================================
// WHAT WAS MISSING, IN ONE SENTENCE
// ================================================================================================
//
// Before this module the engine could DESCRIBE a skin (`render::geometry::SkinningDescriptor`),
// could compute its animated BOUNDS (`skinned_bounds`), could store its BLEND SHAPES
// (`BlendShapeSet`), could evaluate a skeleton to a pose and publish the skinning matrices
// (`cy::animation::PoseWorld`, `publish_pose`) — and could not move a single vertex. There was no
// skinning shader in the tree and no CPU skinning either; `SkinningDescriptor` was constructed
// nowhere outside its own tests. This module is the arrow from a bone matrix to a moved vertex.
//
// ================================================================================================
// THE OUTPUT IS A VERTEX BUFFER, WHICH IS THE WHOLE POINT OF THE REQUIREMENT
// ================================================================================================
//
// "so the result is reusable across passes (depth, shadows, main) without re-skinning". The
// positions this pass writes are `render::VertexStream::Position` at stride 12 and the frames are
// `::NormalTangent` at stride 8 — the bytes a vertex input binds, in a buffer created with
// `BufferUsage::Vertex | BufferUsage::Storage`. A depth prepass, four shadow cascades and the
// opaque pass bind the same range; nothing repacks and nothing skins twice.
//
// THE BARRIER BETWEEN THE DISPATCH AND THE DRAW IS THE GRAPH'S. `declare()` adds one compute pass
// that WRITES the output buffers; a caller's draw pass READS them with
// `Access::VertexAttributeRead` and the graph derives the transition. This module emits no barrier
// of its own, which is the same arrangement `GpuCullPass` documents — and it is why a skinning
// dispatch attached as a `PassExtension` on the Prepare stage would be wrong: an extension sits
// outside the graph's declared resources and no barrier would be derived for what it wrote.
//
// ================================================================================================
// DOUBLE BUFFERING, AND THE ONE FRAME AT A TIME THAT IS NOT DOUBLE BUFFERING
// ================================================================================================
//
// "Output buffers SHALL be double buffered so the previous frame's positions are available for
// motion vectors." One buffer holds both frames' vertices: `SkinnedBuffers::for_frame(index)` picks
// the parity, `skin(frame_index)` writes that half, and `previous_vertex_offset()` is where the
// other half begins. The pair is indexed by parity rather than swapped through a pointer because
// several passes in one frame must agree about which half is current, and a swap between two of
// them would give the depth prepass one set of vertices and the opaque pass another.
//
// THAT IS NOT THE SAME AS FRAMES IN FLIGHT, and conflating the two is how M3's recycled descriptor
// set happened. One `SkinPass` owns one descriptor set naming one set of buffers; submitting a
// second frame's dispatch before the first has completed is a genuine write-after-write the graph
// cannot see, because it has no cross-frame state for an imported buffer. The remedy is a pass per
// frame in flight — and NOT a barrier this module could emit. `tests/test_skin_pass.cpp` drains
// each frame before beginning the next, and says so.
//
// ================================================================================================
// WHAT IT REFUSES
// ================================================================================================
//
// Everything `make_skin_constants` refuses: dual quaternion skinning, blend shapes, a baked tier,
// and every rule `SkinningDescriptor::validate()` already carries. The refusals are by name and at
// `upload()`, not at `declare()`, so a caller learns before a command buffer exists.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/graph/graph.h>
#include <cy/servers/render/geometry/skin_dispatch.h>

namespace cy::rendering::skinning {

/// How large a skin the pass is sized for. One allocation each, at creation.
///
/// Sized once and reused, because the point of a skinning pass is that the CPU does no per-vertex
/// work and a pass that reallocated per frame would put the allocator back on the frame path.
struct SkinPassDescription {
    /// One past the highest vertex index any dispatch will cover. The output buffers are twice
    /// this, because the output is double buffered.
    u32 max_vertices = 0;
    /// How many bone matrices the pose buffer holds. The whole GPU pose world when the source is
    /// the shared world — `pose_offset` then selects this skin's slice — and one skeleton's worth
    /// when it is not.
    u32 max_bones = 0;
    /// Whether the pass carries the normal-tangent streams at all. False for a shadow-only skin,
    /// which binds `render::kDepthPassStreams` and reads no frame — and the buffers are then not
    /// created rather than created and ignored.
    bool with_frames = true;
    /// Whether the pass creates host-visible copies of its output and declares the transfer that
    /// fills them.
    ///
    /// OFF BY DEFAULT, because a shipped frame draws from the device-local buffer and never reads
    /// it back — a read-back every frame would be a full copy of every skinned vertex for nobody.
    /// The suite that compares the dispatch against `cpu_reference_skin` turns it on, which is the
    /// only way that comparison can be a buffer rather than a photograph.
    bool read_back = false;
};

/// One skin's dispatch.
///
/// The lifecycle is: `create` once, then per frame `upload` the bind-pose streams (once, for a
/// static mesh) and the pose (every frame), `declare` the pass into the frame's graph, execute the
/// graph, and draw from `output_positions()` / `output_frames()` at `vertex_offset()`.
class SkinPass {
public:
    SkinPass() = default;
    ~SkinPass();

    SkinPass(const SkinPass&) = delete;
    SkinPass& operator=(const SkinPass&) = delete;
    SkinPass(SkinPass&&) = delete;
    SkinPass& operator=(SkinPass&&) = delete;

    /// Whether the device this pass would run on can run it at all.
    ///
    /// A device without compute has no skinning path in this engine at all: `cpu_reference_skin` is
    /// the same answer and would serve as one, and nothing calls it that way today — which is
    /// stated here rather than implied by an absent branch.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    /// Create the pipeline and the buffers. Fails naming the capability when the device has no
    /// compute queue, and naming the field when a size is zero.
    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const SkinPassDescription& desc) noexcept;

    /// Write the bind-pose streams. Once per mesh, not once per frame: these are the cooked
    /// vertices and they do not change because the character moved.
    ///
    /// `frames` may be empty when the pass was created without them, and must not be otherwise.
    [[nodiscard]] Status upload_mesh(
        Span<const Vec3> positions, Span<const render::PackedNormalTangent> frames,
        Span<const render::geometry::GpuSkinInfluence> influences) noexcept;

    /// Write the pose and derive the constant block from the descriptor.
    ///
    /// `skinning_matrices` is what `cy::animation::PoseWorld::matrices()` holds — `model *
    /// inverse_bind` per bone — and `descriptor.pose_offset` is `PoseWorld::matrix_offset(handle)`,
    /// which MOVES EVERY PUBLISH because the world is double buffered. Passing a cached offset
    /// reads the previous frame's bones and nothing can detect it, which is why this takes the
    /// descriptor rather than remembering one.
    ///
    /// `frame_index` picks which half of the output the dispatch writes, through
    /// `SkinnedBuffers::for_frame`.
    [[nodiscard]] Status upload(const render::geometry::SkinningDescriptor& descriptor,
                                Span<const Mat4> skinning_matrices, u64 frame_index) noexcept;

    /// Declare the dispatch into `graph`. Call between `upload` and the graph's execution.
    ///
    /// The output buffers are declared as WRITTEN, so a caller's draw pass that declares them read
    /// with `Access::VertexAttributeRead` gets the barrier derived. The resource identifiers are
    /// `positions_resource()` and `frames_resource()`, valid until the next `declare`.
    [[nodiscard]] Status declare(RenderGraph& graph) noexcept;

    /// The output buffers, for a draw. Both carry `BufferUsage::Vertex`.
    [[nodiscard]] rhi::BufferHandle output_positions() const noexcept { return buffers_.positions; }
    [[nodiscard]] rhi::BufferHandle output_frames() const noexcept { return buffers_.frames; }
    /// The graph resources the last `declare` created for them, so a draw pass can depend on the
    /// dispatch rather than hope the submission order is enough.
    [[nodiscard]] ResourceId positions_resource() const noexcept { return positions_id_; }
    [[nodiscard]] ResourceId frames_resource() const noexcept { return frames_id_; }

    /// The first vertex of the half this frame's dispatch wrote — the `vertex_offset` a
    /// `draw_indexed` takes — and the first vertex of the other half, which holds the previous
    /// frame's positions and is what motion vectors read.
    [[nodiscard]] u32 vertex_offset() const noexcept { return constants_.first_output_vertex; }
    [[nodiscard]] u32 previous_vertex_offset() const noexcept { return previous_offset_; }

    /// The constant block the dispatch was given. What a test hands `cpu_reference_skin` so that
    /// the two run over the same description rather than over two that agree by construction.
    [[nodiscard]] const render::geometry::GpuSkinConstants& constants() const noexcept {
        return constants_;
    }

    /// The skinned vertices, read back off the device.
    ///
    /// MUST be called after the frame's fence has been waited on, and only on a pass created with
    /// `SkinPassDescription::read_back`. Present so the dispatch can be compared against the
    /// reference by buffer rather than by photograph.
    [[nodiscard]] Expected<Span<const Vec3>, Error> read_back_positions() noexcept;
    [[nodiscard]] Expected<Span<const render::PackedNormalTangent>, Error>
    read_back_frames() noexcept;

    void destroy() noexcept;

private:
    struct Buffers {
        rhi::BufferHandle bones;
        rhi::BufferHandle in_positions;
        rhi::BufferHandle in_frames;
        rhi::BufferHandle influences;
        rhi::BufferHandle positions;
        rhi::BufferHandle frames;
        rhi::BufferHandle positions_readback;
        rhi::BufferHandle frames_readback;
    };

    [[nodiscard]] Status create_pipeline() noexcept;
    [[nodiscard]] Status create_buffers() noexcept;
    [[nodiscard]] Status write_descriptors() noexcept;

    static void record_skin(const PassContext& context, void* user) noexcept;
    static void record_readback(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    SkinPassDescription desc_{};
    render::geometry::GpuSkinConstants constants_{};
    u32 previous_offset_ = 0;
    bool mesh_uploaded_ = false;

    rhi::ShaderModuleHandle shader_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle pipeline_layout_;
    rhi::ComputePipelineHandle pipeline_;
    rhi::DescriptorSetHandle descriptor_set_;
    Buffers buffers_{};
    ResourceId positions_id_ = kInvalidResource;
    ResourceId frames_id_ = kInvalidResource;
};

}  // namespace cy::rendering::skinning
