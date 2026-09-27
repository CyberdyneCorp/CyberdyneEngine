// SPDX-License-Identifier: MIT
#pragma once
// Selection outlines on the device: the mask of what a game marked, and the edge pass that
// composites their outlines over the tonemapped colour.
//
// ================================================================================================
// THE SHAPE, AND WHERE IT SITS IN THE FRAME
// ================================================================================================
//
//   post-process ─► output ─┐
//                           ├─► composite (edge pass, blended over the output) ─► ui and debug
//   marked draws ─► mask ───┘        ▲
//                   (slot + depth)   └── prepass depth (hidden or visible)
//
// Two graph passes, declared at the frame's own `FramePassKind::SelectionOutlines` stage through
// `FrameStageDeclaration`: a raster pass that draws the frame's opaque draws whose stable identity
// is marked — through the frame's own sets 0 and 1 and its position stream — into a transient
// `R32Uint` mask and a transient depth, and a full-screen pass that reads both with the prepass
// depth and blends onto the stage's target. Both targets are graph transients: nothing outside this
// module samples them, so unlike the contact term they need no slot in the frame's texture table.
//
// With nothing marked the stage is still declared and records nothing, and the frame is the frame
// without it, byte for byte (`render.selection_outlines`, case (e)).
//
// ================================================================================================
// WHAT A CALLER DOES
// ================================================================================================
//
//     pass.create(device, pipelines, {width, height});
//     // per frame, with `AssemblyDescription::selection_outlines`:
//     pass.set_highlights(&highlights, settings);
//     sinks.selection_outlines = pass.stage(recorder);
//
// `recorder` is the `pipeline::FrameRecorder` that records the frame: the mask draws what it draws,
// through its geometry and its bindings, so the two cannot disagree about where an object is.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/pipeline/frame_recorder.h>
#include <cy/rendering/selection/highlight.h>

namespace cy::rendering::selection {

struct OutlinePassDescription {
    /// The frame's extent.
    u32 width = 0;
    u32 height = 0;
    /// Whether `read_back` may be called: host copies of the mask, the mask depth and the scene
    /// depth, which a test compares with the host reference.
    bool readback = false;
};

/// What the last recorded frame did.
struct OutlineReport {
    /// Draws whose identity was marked and which the mask drew.
    u32 marked_draws = 0;
    /// Marked draws the mask could not draw: no geometry yet, or a draw index past 24 bits.
    u32 skipped_draws = 0;
    /// Whether the composite ran. False when nothing marked was drawn.
    bool composited = false;
};

/// The mask and the depths the edge pass read, as `read_back` returns them.
struct OutlineReadback {
    Span<u32> mask;
    Span<f32> mask_depth;
    Span<f32> scene_depth;
};

class OutlinePass {
public:
    OutlinePass() = default;
    ~OutlinePass();

    OutlinePass(const OutlinePass&) = delete;
    OutlinePass& operator=(const OutlinePass&) = delete;
    OutlinePass(OutlinePass&&) = delete;
    OutlinePass& operator=(OutlinePass&&) = delete;

    /// A native shader format this module ships: SPIR-V or MSL.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    /// `pipelines` are the frame's: the mask pipeline's layout names their sets 0 and 1 and their
    /// push range, and draws in their depth format.
    [[nodiscard]] Status create(rhi::Device& device, const pipeline::FramePipelines& pipelines,
                                const OutlinePassDescription& desc) noexcept;
    void destroy() noexcept;

    /// What this frame outlines, and how. The set is read when the stage is declared and when it
    /// records, so it must outlive the frame's execution. Null outlines nothing.
    [[nodiscard]] Status set_highlights(const HighlightSet* highlights,
                                        const OutlineSettings& settings) noexcept;
    [[nodiscard]] const OutlineSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] const OutlineConstants& constants() const noexcept { return constants_; }

    /// The frame's hook: the stage declared as this module's two passes, drawing through
    /// `recorder`.
    [[nodiscard]] FrameStageDeclaration stage(const pipeline::FrameRecorder& recorder) noexcept;

    /// Declare the mask and the composite. Returns the mask pass, or `kInvalidPass` when the
    /// inputs are not the ones this pass was created for.
    [[nodiscard]] PassId declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept;

    [[nodiscard]] const OutlineReport& report() const noexcept { return report_; }

    /// The last executed frame's mask and depths, each `width * height`, row-major from the
    /// top-left. Call after its fence.
    [[nodiscard]] Status read_back(const OutlineReadback& out) const noexcept;

    // Public because `RecordFn` is a plain function pointer and the callbacks are free functions.
    void record_mask(const PassContext& context) noexcept;
    void record_composite(const PassContext& context) noexcept;
    void record_readback(const PassContext& context) noexcept;

private:
    [[nodiscard]] Status create_mask_pipeline(const pipeline::FramePipelines& pipelines) noexcept;
    [[nodiscard]] Status create_composite_pipeline() noexcept;
    [[nodiscard]] Status create_buffers() noexcept;
    [[nodiscard]] Status upload_styles() noexcept;
    /// The marked opaque draws, into the open mask scope.
    void draw_marked(rhi::CommandBuffer& commands) noexcept;
    static PassId declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                void* user) noexcept;

    rhi::Device* device_ = nullptr;
    OutlinePassDescription desc_{};
    const HighlightSet* highlights_ = nullptr;
    const pipeline::FrameRecorder* recorder_ = nullptr;
    OutlineSettings settings_{};
    OutlineConstants constants_{};
    OutlineReport report_{};

    rhi::ShaderModuleHandle mask_vertex_;
    rhi::ShaderModuleHandle mask_fragment_;
    rhi::ShaderModuleHandle composite_vertex_;
    rhi::ShaderModuleHandle composite_fragment_;
    rhi::PipelineLayoutHandle mask_layout_;
    rhi::GraphicsPipelineHandle mask_pipeline_;
    rhi::DescriptorSetLayoutHandle composite_set_;
    rhi::PipelineLayoutHandle composite_layout_;
    rhi::GraphicsPipelineHandle composite_pipeline_;
    rhi::Format depth_format_ = rhi::Format::D32Sfloat;
    rhi::Format output_format_ = rhi::Format::Rgba8Unorm;

    /// The style palette, one host-visible buffer per frame in flight.
    rhi::BufferHandle styles_[rhi::kMaxFramesInFlight];
    u32 style_slots_ = 0;
    u32 style_ring_ = 0;
    rhi::BufferHandle readbacks_[3];

    // This frame's resources, from `declare`.
    ResourceId mask_ = kInvalidResource;
    ResourceId mask_depth_ = kInvalidResource;
    ResourceId scene_depth_ = kInvalidResource;
    ResourceId target_ = kInvalidResource;
};

}  // namespace cy::rendering::selection
