#pragma once
// The hierarchical depth buffer, ON THE DEVICE. M11.c task 4.1.
//
// `rendering-culling-and-lod` — "Occlusion culling": "The engine SHALL support occlusion culling
// using a **hierarchical depth buffer** built from the previous frame's depth, reprojected into the
// current view."
//
// ================================================================================================
// WHY THIS MODULE EXISTS AT ALL, AND WHY IT IS NOT IN EITHER MODULE THAT WANTED IT
// ================================================================================================
//
// Two rows were waiting on exactly one piece of work and the plan said so before it was built:
// `rendering-culling-and-lod`'s occlusion cull — `GpuCullPass::upload` refused `kGpuCullOcclusion`
// by name because no pyramid existed on a device — and `virtual-geometry`'s cluster-granular
// occlusion, whose seam was already cut with `TraversalStatistics::nodes_pruned_by_occlusion`
// reading zero. Building the pyramid inside either one would have put the other's dependency on a
// module it has no reason to link, and building it twice would have let ONE piece of work be
// recorded as TWO satisfied requirements. That is the failure mode M11.c's own specification delta
// names in as many words, so the pyramid is its own module and both consumers link it.
//
// ================================================================================================
// THE PYRAMID IS A BUFFER, AND THAT IS THIS ENGINE'S SHAPE RATHER THAN A SHORTCUT
// ================================================================================================
//
// Every compute path in this tree already carries depth in a buffer: `vg_visbuffer.slang` settles
// depth and payload in one 64-bit atomic over an `RWStructuredBuffer`, and the compute rasteriser
// is the path virtual geometry ships. So the pyramid is `width * height + ...` floats, levels
// concatenated coarsest last, and a consumer reads it with one `StructuredBuffer<float>` binding
// and no sampler, no image layout and no per-mip view.
//
// WHAT THAT COSTS, stated rather than discovered: a texture pyramid would sample with hardware
// filtering and would be laid out for two-dimensional locality. This one is read with four explicit
// loads at a level chosen so the footprint spans at most a 2x2, which is the same four taps
// `Hzb::occludes` makes — so the ANSWER is identical and the cache behaviour is not. That is a
// performance property and it is recorded in this module's README rather than left to be found.
//
// ================================================================================================
// THE CPU MODEL IS THE EXPECTED VALUE, NOT A SECOND IMPLEMENTATION
// ================================================================================================
//
// `cy::render::culling::Hzb` (src/servers/render/culling/include/cy/servers/render/culling/hzb.h)
// is a CPU model of this structure and has been tested as one since M6. `hzb_reduce.slang`'s
// reduction is `Hzb::reduce()` transcribed expression by expression, including the fold that takes
// an odd dimension's extra row and column into the same parent texel; `hzb_sample.slang`'s test is
// `project_sphere` and `Hzb::occludes` transcribed the same way. `read_back()` fills a model object
// from the device pyramid so that a test compares TEXELS rather than counters, and
// `integration.rendering_culling` is where that comparison lives.
//
// A MODEL PASSING ITS OWN TESTS IS NOT A PASS. That sentence is the requirement M11.c adds to
// `rendering-culling-and-lod`, and it is why this class has a `read_back` at all.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/graph/graph.h>
#include <cy/servers/render/culling/hzb.h>

namespace cy::rendering::hzb {

/// How many floats a pyramid of this size occupies, levels concatenated.
///
/// `render::culling::hzb_level_count` decides how many levels there are and this sums their areas,
/// so the two cannot drift: a consumer that walks the levels in a shader recomputes exactly this
/// series with the same `halved()`.
[[nodiscard]] u32 hzb_total_texels(u32 width, u32 height) noexcept;

/// The first texel of one level within that buffer.
[[nodiscard]] u32 hzb_level_offset(u32 width, u32 height, u32 level) noexcept;

/// The occlusion parameters a CONSUMER's dispatch reads, as a push-constant block.
///
/// Eighty bytes, and every consumer in the tree spells it the same way — `gpu_cull.slang` and
/// `vg_traversal.slang` both include `hzb_sample.slang`, which declares this struct once. A second
/// spelling would be a second chance to transpose a matrix.
///
/// `view_projection` is COLUMN MAJOR, four columns of four, because `cy::Mat4` is column-major with
/// column vectors and the projection in a shader is written out as
/// `c0 * x + c1 * y + c2 * z + c3` rather than as a `mul` whose operand order is a convention.
struct alignas(16) HzbParams {
    f32 view_projection[4][4] = {{1.0F, 0.0F, 0.0F, 0.0F},
                                 {0.0F, 1.0F, 0.0F, 0.0F},
                                 {0.0F, 0.0F, 1.0F, 0.0F},
                                 {0.0F, 0.0F, 0.0F, 1.0F}};
    u32 width = 0;
    u32 height = 0;
    u32 level_count = 0;
    /// Zero answers "not occluded" for everything, whatever the pyramid holds. What a camera cut
    /// leaves behind, and what a consumer with no pyramid attached sends: `Hzb::invalidate()` on
    /// the device, spelled as data because a dispatch cannot branch on a null pointer.
    u32 enabled = 0;
};

static_assert(sizeof(HzbParams) == 80, "HzbParams is a shader-visible push-constant block");

/// Fill a parameter block from a view-projection and a pyramid's dimensions.
[[nodiscard]] HzbParams hzb_params(const Mat4& view_projection, u32 width, u32 height,
                                   u32 level_count, bool enabled) noexcept;

struct HzbPassDescription {
    /// The resolution of level 0, which is the resolution of the depth the pyramid is built from.
    u32 width = 0;
    u32 height = 0;
    /// Whether `read_back()` may be called. A readback buffer the size of the pyramid is a real
    /// cost and a shipped frame never reads the pyramid on the host — it is what makes the device
    /// pass comparable against the CPU model, so it is requested rather than always present.
    bool readback = false;
};

/// The pyramid, its reduction chain, and the buffer both consumers bind.
///
/// Lifecycle: `create` once, then per frame `declare(graph, depth)` between the pass that wrote the
/// depth and the passes that test against the pyramid. The barriers between the levels are the
/// graph's, which is why there is not one barrier call in this module.
class HzbPass {
public:
    HzbPass() = default;
    ~HzbPass();

    HzbPass(const HzbPass&) = delete;
    HzbPass& operator=(const HzbPass&) = delete;
    HzbPass(HzbPass&&) = delete;
    HzbPass& operator=(HzbPass&&) = delete;

    /// Whether this device can build the pyramid. A device without compute gets the CPU model,
    /// which is `render::culling::Hzb` and is the same answer.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const HzbPassDescription& desc) noexcept;

    /// Declare the seed copy and the `level_count - 1` reduction dispatches into `graph`.
    ///
    /// `depth` is a graph resource holding `width * height` floats in row-major order, in the
    /// engine's reversed-Z [0, 1] convention — 1 at the near plane. It is read as a transfer
    /// source, so the caller's buffer needs `TransferSource` usage; the resource is the CALLER's
    /// because the depth belongs to whatever pass produced it and importing it here twice would
    /// give the graph two names for one buffer.
    ///
    /// RETURNS THE PYRAMID'S RESOURCE ID, AND A CONSUMER MUST BE HANDED IT RATHER THAN IMPORTING
    /// THE BUFFER ITSELF. `RenderGraph::import_buffer` does not de-duplicate: a second import of
    /// the same handle is a SECOND resource, with no edge between the reduction that writes it and
    /// the dispatch that reads it — so the graph derives no barrier, and the answer is right on
    /// some frames and "nothing was occluded" on others. `GpuCullPass::declare` takes this id for
    /// exactly that reason, and refuses when a pyramid is attached and no id was passed.
    [[nodiscard]] Expected<ResourceId, Error> declare(RenderGraph& graph,
                                                      ResourceId depth) noexcept;

    /// Mark the pyramid built. `valid()` is what `params()` reports as `enabled`, and it exists for
    /// the same reason `Hzb::mark_valid()` does: a pyramid nobody has filled must occlude nothing.
    void mark_valid() noexcept { valid_ = true; }
    /// A camera cut. Every test then answers "not occluded" until the next `declare`+`mark_valid`.
    void invalidate() noexcept { valid_ = false; }
    [[nodiscard]] bool valid() const noexcept { return valid_; }

    /// The parameters a consumer pushes, for this pyramid and this view.
    [[nodiscard]] HzbParams params(const Mat4& view_projection) const noexcept;

    /// The buffer a consumer binds. One `StructuredBuffer<float>`, levels concatenated.
    [[nodiscard]] rhi::BufferHandle buffer() const noexcept { return pyramid_; }

    [[nodiscard]] u32 width() const noexcept { return desc_.width; }
    [[nodiscard]] u32 height() const noexcept { return desc_.height; }
    [[nodiscard]] u32 level_count() const noexcept { return level_count_; }

    /// Copy the device pyramid into the CPU model, level for level.
    ///
    /// MUST be called after the frame's fence has been waited on. Fails when the pass was created
    /// without `readback`, rather than handing back a model full of whatever the host buffer held:
    /// an empty expected value is the one answer a comparison cannot misread.
    [[nodiscard]] Status read_back(render::culling::Hzb& model) const noexcept;

    void destroy() noexcept;

private:
    /// One reduction step's push constants, and the state its record callback needs.
    struct Reduce {
        u32 source_offset = 0;
        u32 destination_offset = 0;
        u32 source_width = 0;
        u32 source_height = 0;
        u32 destination_width = 0;
        u32 destination_height = 0;
        u32 reserved0 = 0;
        u32 reserved1 = 0;
    };
    static_assert(sizeof(Reduce) == 32, "the reduction push block is eight words");

    struct Step {
        HzbPass* self = nullptr;
        Reduce push{};
    };

    [[nodiscard]] Status create_pipeline() noexcept;
    [[nodiscard]] Status create_buffers() noexcept;
    [[nodiscard]] Status write_descriptors() noexcept;

    static void record_seed(const PassContext& context, void* user) noexcept;
    static void record_reduce(const PassContext& context, void* user) noexcept;
    static void record_readback(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    /// The depth resource the last `declare` was given. Read through the executor when the seed
    /// pass records, never captured as a handle at declaration time.
    ResourceId seed_depth_ = kInvalidResource;
    HzbPassDescription desc_{};
    u32 level_count_ = 0;
    u32 total_texels_ = 0;
    bool valid_ = false;

    rhi::BufferHandle pyramid_;
    rhi::BufferHandle readback_;
    rhi::ShaderModuleHandle shader_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle pipeline_layout_;
    rhi::ComputePipelineHandle pipeline_;
    rhi::DescriptorSetHandle descriptor_set_;

    /// One per reduction, kept for the life of the pass because a record callback is a function
    /// pointer and a `void*` and the lifetime of what it points at is this object's. Sized at
    /// creation: `render::culling::hzb_level_count` caps it and the cap is checked there.
    static constexpr u32 kMaxLevels = 20;
    Step steps_[kMaxLevels]{};
};

}  // namespace cy::rendering::hzb
