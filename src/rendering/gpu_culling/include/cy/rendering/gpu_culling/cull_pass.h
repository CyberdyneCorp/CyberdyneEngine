#pragma once
// The culling compute pass: the dispatch `cpu_reference_cull` was written to be checked against.
// M7 task 5.1.
//
// `rendering-culling-and-lod` — "GPU-driven culling": "Where the device supports compute and
// indirect drawing, the renderer SHALL support GPU-driven culling: instance bounds are uploaded
// once, culling runs as a compute pass producing compacted draw arguments, and drawing uses
// indirect commands."
//
// ================================================================================================
// WHAT M6 LEFT, AND WHAT THIS FILE IS
// ================================================================================================
//
// M6 delivered `src/servers/render/culling/` — the records a dispatch reads and writes, and a CPU
// implementation of exactly the algorithm the shader runs. Its closing gate recorded the gap in one
// sentence: `cy::servers-render-culling` was linked by nothing but its own test binaries. This
// module is the other half. It owns a device, a compute pipeline and the buffers, it runs the
// dispatch, and `tests/test_gpu_cull_pass.cpp` compares its output against the reference's BY
// COMPARING BUFFERS — the commands word for word, the payloads field by field, the counters, the
// cluster list and the LOD hysteresis state.
//
// ================================================================================================
// WHY THIS IS AT LAYER 4 AND THE RECORDS ARE AT LAYER 2
// ================================================================================================
//
// A compute pass needs a device and a render graph, and `src/servers/` may name neither. The split
// is `gpu_cull.h`'s and this file only obeys it: the LAYOUTS are underneath the device because a
// backend has to be able to read them, and the DISPATCH is above it because it calls one.
//
// ================================================================================================
// THE BUFFERS ARE PERSISTENT AND THE PASS IS NOT
// ================================================================================================
//
// `GpuCullPass` owns its buffers for its whole life and imports them into the graph each frame. The
// alternative — graph transients — would need the descriptor set rewritten every frame, because a
// transient's handle is a per-frame thing and a descriptor written once cannot name it. That is the
// same decision `samples/03-first-light` records for its shadow map, for the same reason.
//
// Sized once at `create()` for the scene's high water mark, because the point of GPU-driven culling
// is that the CPU does no per-instance work, and a pass that reallocated per frame would put the
// allocator back on the frame path.
//
// ONE SET OF BUFFERS MEANS ONE FRAME AT A TIME. A caller that submits a second frame's dispatch
// before the first has completed has two dispatches writing one counters buffer with no barrier
// between them, because the graph has no cross-frame state for an imported buffer and cannot know
// there was a previous frame. That is a genuine write-after-write and validation reports it. The
// remedy is a pass per frame in flight — two objects, two sets of buffers — and NOT a barrier this
// module could emit, because barriers are the graph's. `tests/test_gpu_cull_pass.cpp`'s teardown
// case drains each frame before beginning the next for exactly this reason, and says so.
//
// ================================================================================================
// WHAT IT REFUSES, AND WHAT M11.c CHANGED ABOUT IT
// ================================================================================================
//
// A view with `kGpuCullOcclusion` set AND NO PYRAMID ATTACHED. Until M11.c task 4.1 there was no
// pyramid to attach: `hzb.h` was a CPU model and nothing built one on a device, so the flag was
// refused unconditionally because a dispatch that quietly ignored it would report "nothing was
// occluded" and be indistinguishable from a working occlusion cull over an empty pyramid.
//
// `cy::rendering-hzb` is now that pyramid, `set_occlusion()` attaches one, and the refusal narrows
// to exactly the case it was written for. It does NOT go away: a caller that asks for occlusion
// culling and hands over no pyramid is asking for a cull that cannot occlude anything, and the
// answer is still an error naming the flag rather than a green frame with nothing culled.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/hzb/hzb_pass.h>
#include <cy/servers/render/culling/gpu_cull.h>

namespace cy::rendering::gpu_culling {

/// How large a scene the pass is sized for. One allocation each, at creation.
struct GpuCullPassDescription {
    /// One past the highest GPU scene slot a dispatch will cover.
    u32 max_instances = 0;
    /// How many draws the output may hold. The scene's high water mark is the safe answer, and it
    /// is what a caller that does not want to think about it should pass.
    u32 max_draws = 0;
    u32 max_lod_chains = 0;
    u32 max_mesh_lods = 0;
    /// Zero when no instance declares a visibility range, which is the common case and is not the
    /// same as a range buffer full of zeroes: the reference treats an EMPTY span as "ranges are not
    /// declared" and a populated one as "they are", and the dispatch is told which through a push
    /// constant rather than guessing from a length.
    u32 max_visibility_ranges = 0;
    /// Zero disables LOD hysteresis on the device however the view is configured — which is what a
    /// deterministic test wants, and what a first frame has.
    u32 max_previous_levels = 0;
};

/// What one dispatch produced, read back. The same four things `GpuCullOutput` holds, in the same
/// order, so that a comparison against the reference is a memcmp and not a translation.
struct GpuCullReadback {
    Span<const render::culling::GpuDrawIndexedIndirect> commands;
    Span<const render::culling::GpuDrawPayload> payloads;
    Span<const u32> virtual_geometry;
    /// The hysteresis state the dispatch wrote back, or an empty span when the pass was created
    /// with none.
    Span<const u32> previous_levels;
    render::culling::GpuCullCounters counters{};
};

/// One view's culling dispatch.
///
/// The lifecycle is: `create` once, then per frame `upload` the scene and the view, `declare` the
/// passes into the frame's graph, execute the graph, and `read_back`. `declare` records nothing
/// itself — it adds two compute passes and two copies, and the graph derives every barrier between
/// them, which is why there is no barrier call anywhere in this module.
class GpuCullPass {
public:
    GpuCullPass() = default;
    ~GpuCullPass();

    GpuCullPass(const GpuCullPass&) = delete;
    GpuCullPass& operator=(const GpuCullPass&) = delete;
    GpuCullPass(GpuCullPass&&) = delete;
    GpuCullPass& operator=(GpuCullPass&&) = delete;

    /// Create the pipelines and the buffers. Fails naming the capability when the device has no
    /// compute queue, and naming the field when a size is zero.
    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const GpuCullPassDescription& desc) noexcept;

    /// Whether the device this pass would run on can run it at all. A device without compute or
    /// without indirect drawing gets the CPU path, which is `cpu_reference_cull` and is the same
    /// answer — that is the whole reason the reference is a reference and not a test fixture.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    /// Attach the hierarchical depth pyramid the occlusion test reads, and the view-projection it
    /// projects bounds through.
    ///
    /// ONE PYRAMID, TWO CONSUMERS. `cy::rendering::hzb::HzbPass` is the same object
    /// `virtual-geometry`'s cluster-granular occlusion reads, and `hzb_sample.slang` is the same
    /// test — which is what stops one piece of device work from being recorded as two satisfied
    /// requirements.
    ///
    /// `view_projection` is the matrix the PYRAMID was built in, not necessarily this view's: a
    /// two-pass scheme rebuilds the pyramid from the first pass's depth and tests the rest against
    /// it, and both halves are the same camera. It is taken here rather than read out of
    /// `GpuCullView` because that block carries a frustum and no matrix, and a frustum cannot
    /// project a sphere onto a depth buffer.
    ///
    /// A null pyramid detaches, which is what a camera cut does alongside `HzbPass::invalidate()`.
    [[nodiscard]] Status set_occlusion(const hzb::HzbPass* pyramid,
                                       const Mat4& view_projection) noexcept;

    /// Copy the scene and the view into the device buffers, and zero the counters.
    ///
    /// Everything here is host-visible and written directly. A staging copy would be the shape for
    /// a scene that changed rarely; a cull's inputs change every frame by definition, so the upload
    /// ring is the right memory and a transfer pass would be a copy of a copy.
    [[nodiscard]] Status upload(const render::culling::GpuCullScene& scene,
                                const render::culling::GpuCullView& view) noexcept;

    /// Declare the two dispatches and the read-back into `graph`. Call between `upload` and the
    /// graph's execution.
    ///
    /// `pyramid` is what `HzbPass::declare` returned for THIS graph, and it is required whenever a
    /// pyramid is attached. It is not an optimisation: `RenderGraph::import_buffer` does not
    /// de-duplicate, so importing the pyramid's handle here would give the graph a SECOND resource
    /// for one buffer, with no edge between the reduction that writes it and this dispatch that
    /// reads it — and a missing barrier on a read-after-write is right on some frames and reports
    /// "nothing was occluded" on others. Passing `kInvalidResource` with a pyramid attached is
    /// refused rather than raced.
    [[nodiscard]] Status declare(RenderGraph& graph,
                                 ResourceId pyramid = kInvalidResource) noexcept;

    /// What the dispatch wrote. Valid until the next `upload`.
    ///
    /// MUST be called after the frame's fence has been waited on — `Device::wait_idle()` or the
    /// frame's own fence — because the read-back buffers are host memory the GPU wrote.
    [[nodiscard]] Expected<GpuCullReadback, Error> read_back() noexcept;

    void destroy() noexcept;

private:
    struct Buffers {
        rhi::BufferHandle view;
        /// One float, bound at the pyramid's binding when no pyramid is attached. Vulkan has no
        /// zero-length buffer and a descriptor must name something; `Sizes::occlusion.enabled` is
        /// what tells the shader the binding is empty.
        rhi::BufferHandle no_pyramid;
        rhi::BufferHandle instances;
        rhi::BufferHandle chains;
        rhi::BufferHandle mesh_lods;
        rhi::BufferHandle ranges;
        rhi::BufferHandle previous_levels;
        rhi::BufferHandle slot_emit;
        rhi::BufferHandle slot_command;
        rhi::BufferHandle slot_payload;
        rhi::BufferHandle counters;
        rhi::BufferHandle commands;
        rhi::BufferHandle payloads;
        rhi::BufferHandle virtual_geometry;
    };

    /// The host-visible half: the sixteen zero words the counters are cleared from, and the four
    /// staging buffers the read-back pass copies into.
    struct Readback {
        rhi::BufferHandle counters_zero;
        rhi::BufferHandle counters;
        rhi::BufferHandle commands;
        rhi::BufferHandle payloads;
        rhi::BufferHandle virtual_geometry;
    };

    /// The eight words the two entry points read as a push constant: the span lengths the CPU knows
    /// and a structured buffer's own length cannot express, because Vulkan has no zero-length
    /// buffer and "no visibility ranges" is a real state.
    struct Sizes {
        u32 chain_count = 0;
        u32 mesh_lod_count = 0;
        u32 range_count = 0;
        u32 previous_level_count = 0;
        u32 draw_capacity = 0;
        u32 reserved0 = 0;
        u32 reserved1 = 0;
        u32 reserved2 = 0;
        /// The occlusion test's own block, and `enabled` is 0 until `set_occlusion` attaches a
        /// pyramid. 112 bytes in total, which is inside the 128 every Vulkan implementation
        /// guarantees.
        hzb::HzbParams occlusion{};
    };
    static_assert(sizeof(Sizes) == 112, "the cull push block must fit the guaranteed 128 bytes");

    [[nodiscard]] Status create_pipelines() noexcept;
    [[nodiscard]] Status create_buffers() noexcept;
    [[nodiscard]] Status write_descriptors() noexcept;

    static void record_clear(const PassContext& context, void* user) noexcept;
    static void record_cull(const PassContext& context, void* user) noexcept;
    static void record_compact(const PassContext& context, void* user) noexcept;
    static void record_readback(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    GpuCullPassDescription desc_{};
    Sizes sizes_{};
    /// The pyramid currently attached, or null. Held so that `declare()` imports the buffer the
    /// descriptor names and the graph derives the barrier between the reduction and this dispatch.
    const hzb::HzbPass* pyramid_ = nullptr;
    u32 instance_count_ = 0;

    rhi::ShaderModuleHandle cull_shader_;
    rhi::ShaderModuleHandle compact_shader_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle pipeline_layout_;
    rhi::ComputePipelineHandle cull_pipeline_;
    rhi::ComputePipelineHandle compact_pipeline_;
    rhi::DescriptorSetHandle descriptor_set_;
    Buffers buffers_{};
    Readback readback_{};
};

}  // namespace cy::rendering::gpu_culling
