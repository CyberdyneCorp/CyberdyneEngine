// SPDX-License-Identifier: MIT
#pragma once
// SPDX-License-Identifier: MIT
// Virtual geometry in the forward frame's pass order, rasterised in hardware. M11.c task 4.3.
//
// ================================================================================================
// WHAT WAS MISSING, AND THE MEASUREMENT THAT SAID SO
// ================================================================================================
//
// Through M11.c's first pass the module's README said: "Nothing in the tree links
// `cy::rendering-virtual-geometry` from `cy::rendering-forward`". The link graph was the
// measurement — the traversal, the visibility buffer and the resolve had device suites and had
// produced the clusters picture, and every one of those pictures was drawn by a HARNESS: a sample's
// own `RenderGraph` with the traversal and `VisbufferPass::record` in it and nothing else. The
// engine's frame — `ForwardFrame`'s pass order — had no stage virtual geometry could be in, and the
// rasteriser that ran was the compute one, which the requirement permits and does not default to:
// visible clusters "SHALL be rasterised through a **hardware** path by default".
//
// ================================================================================================
// WHAT THIS IS
// ================================================================================================
//
// The hardware rasteriser, and the thing that puts it in the frame:
//
//   * `ForwardFrame` declares a `virtual geometry` stage after the depth prepass when
//     `FrameFeatures::virtual_geometry` is on, with a one-word visibility target and the frame's
//     OWN depth buffer as its attachments (cy/rendering/forward/frame.h says why there).
//   * `build()` below declares virtual geometry's head passes, then the forward frame with this
//     class's record callback in that stage, then the gather and virtual geometry's resolve chain
//     over what the stage wrote. The stage draws ONE indexed-indirect draw whose instances are the
//     visible clusters; its vertex shader decodes the SAME cluster payload through the SAME
//     `decodeVertex` the compute rasteriser uses, and its fragment shader writes the SAME
//     `(identity << 8) | triangle` word. The depth test is the frame's depth attachment instead of
//     a 64-bit atomic — and because it is the frame's depth, a mesh the prepass drew and a cluster
//     occlude one another.
//
// So the visibility buffer the resolve reads is a pure function of which rasteriser ran, and
// `render.virtual_geometry_forward` compares the two over one traversal.
//
// ================================================================================================
// WHAT IT DOES NOT DO, SAID HERE RATHER THAN DISCOVERED
// ================================================================================================
//
// * **It does not shade into the frame's colour.** The resolve chain runs and fills
//   `VisbufferReadback::resolved`, exactly as it does for the compute rasteriser; no stage of the
//   forward frame reads the visibility target to write `FrameResources::color`. The opaque pass's
//   material path is `cy::rendering-pipeline`'s, and a material resolve that evaluates THAT
//   program per bin is the next rung — not a pass this class could add without owning a shading
//   model, which `FrameAssembly` already refuses to.
// * **An exact depth tie is broken by draw order, not by identity.** The compute path settles depth
//   and payload in one atomic, so a tie goes to the lower identity on every run. A depth attachment
//   keeps whichever fragment passed `GreaterOrEqual` last, and the instances are the visible list,
//   whose order is atomic-append order. Two clusters of one cut never overlap, so a tie needs two
//   coincident surfaces; `render.virtual_geometry_forward` bounds every pixel on which the two
//   rasterisers disagree, a tie included, and `render.virtual_geometry_gpu`'s tie case remains a
//   statement about the compute path alone.
// * **It does not run on `FrameAssembly`.** `build()` is given a `ForwardFrame` and declares it; a
//   frame assembled through `cy::rendering-assembly` reaches the same stage through
//   `FrameSinks::passes[FramePassKind::VirtualGeometry]`, and nothing in the tree assembles one
//   yet.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/virtual_geometry/gpu.h>
#include <cy/rendering/virtual_geometry/visbuffer.h>

namespace cy::rendering::vg {

/// What the stage recorded, read off the recording rather than predicted.
struct ForwardVisibilityReport {
    /// Times the `virtual geometry` stage's callback ran. Zero after an executed frame means the
    /// stage was culled or never declared, and the visibility buffer came from nothing.
    u32 stages_recorded = 0;
    /// Indirect draws recorded — one per recorded stage.
    u32 draws = 0;
    /// Whether the last recorded stage LOADED the depth a prepass wrote rather than clearing it.
    bool loaded_prepass_depth = false;
};

/// The hardware rasteriser and the frame it runs in. One per view, like `VisbufferPass`.
class ForwardVisibility {
public:
    explicit ForwardVisibility(rhi::Device& device) noexcept;
    ~ForwardVisibility();

    ForwardVisibility(const ForwardVisibility&) = delete;
    ForwardVisibility& operator=(const ForwardVisibility&) = delete;
    ForwardVisibility(ForwardVisibility&&) = delete;
    ForwardVisibility& operator=(ForwardVisibility&&) = delete;

    /// Create the graphics pipeline over `visbuffer`'s descriptor set, for a frame whose depth is
    /// `depth_format`. `visbuffer` must have been initialised and must outlive this object.
    [[nodiscard]] Status initialise(VisbufferPass& visbuffer, rhi::Format depth_format) noexcept;

    /// Declare one frame into `graph`: virtual geometry's clear and argument passes, then `frame`
    /// built from `description` with the `virtual geometry` stage turned on and recorded here, then
    /// the gather and the resolve chain over the stage's visibility target.
    ///
    /// `traversal` must already have been recorded INTO THIS GRAPH — the stage's vertex shader
    /// reads what that recording writes, and the graph derives the barrier only from the
    /// traversal's own resources. Refused, by name, otherwise.
    ///
    /// Refused as well when `description` is multisampled, sized differently from the visibility
    /// pass, or already carries a `virtual geometry` callback of its own.
    [[nodiscard]] Status build(RenderGraph& graph, const GpuTraversal& traversal,
                               const Mat4& world_to_clip, ForwardFrame& frame,
                               const FrameDescription& description) noexcept;

    [[nodiscard]] const ForwardVisibilityReport& report() const noexcept { return report_; }
    void reset_report() noexcept { report_ = ForwardVisibilityReport{}; }

private:
    static void record_stage(const PassContext& context, void* user) noexcept;
    static void record_gather(const PassContext& context, void* user) noexcept;
    static void record_index_upload(const PassContext& context, void* user) noexcept;

    /// The traversal's five buffers, the draw arguments and the corner indices.
    static constexpr usize kStageReads = 7;

    rhi::Device& device_;
    VisbufferPass* visbuffer_ = nullptr;
    ForwardFrame* frame_ = nullptr;
    FrameDescription description_{};
    /// Whether this frame's stage clears depth — true unless a prepass declared and recorded it.
    bool clear_depth_ = true;
    VisbufferPass::FrameImports imports_{};
    /// The resources the stage reads, handed to the frame through `FramePassCallback::reads`.
    FrameResourceRead reads_[kStageReads] = {};

    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragment_;
    rhi::PipelineLayoutHandle layout_;
    rhi::GraphicsPipelineHandle pipeline_;
    /// `0 .. max_cluster_indices - 1`: an index is a corner of a triangle slot.
    rhi::BufferHandle indices_;
    rhi::BufferHandle index_staging_;
    u64 index_bytes_ = 0;
    /// Set when the copy into `indices_` has been RECORDED, which is when the first graph that
    /// declared it executed — not when it was declared.
    bool indices_uploaded_ = false;
    ForwardVisibilityReport report_;
};

}  // namespace cy::rendering::vg
