// SPDX-License-Identifier: MIT
#pragma once
// Depth of field on the device: five compute dispatches at the frame's
// `FramePassKind::DepthOfField` stage, step 7 of `rendering-post-processing`'s chain.
//
// ================================================================================================
// THE SHAPE, AND WHERE IT SITS IN THE FRAME
// ================================================================================================
//
//   temporal ─► source ─┬─► setup (half res: colour + signed CoC) ─► tiles ─► dilate
//                       │         │                                            │
//            depth ─────┤         └───────────────► gather (far, near) ◄──────┘
//                       │                                   │
//                       └──────────────────────► composite ─► target ─► bloom ─► post-process
//
// Declared by this module through `FrameStageDeclaration`: the frame hands it the colour the chain
// has reached (`ScreenSpaceStageInputs::source`), the single-sample depth, and the full-resolution
// target every later stage reads (`FrameResources::depth_of_field`). The four intermediate images
// are graph transients; nothing outside this module samples them.
//
// Scene-referred, before bloom and exposure: a defocused highlight is a disc of the light it
// carried, and bloom then glows around the disc rather than the disc being cut out of a glow.
//
// ================================================================================================
// WHAT A CALLER DOES
// ================================================================================================
//
//     pass.create(device, {width, height});
//     pass.set_settings(settings);                   // the lens: focal length, f-number, focus
//     // per frame, with `PostChainConfig::depth_of_field`:
//     pass.set_view({projection, width, height});   // the matrix the depth was written with
//     sinks.depth_of_field = pass.stage();
//
// D3D12 is not a target: nothing in this module embeds DXIL, as for every device pass in the tree.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/depth_of_field/focus.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/graph/graph.h>

namespace cy::rendering::depth_of_field {

struct DepthOfFieldPassDescription {
    /// The frame's extent.
    u32 width = 0;
    u32 height = 0;
};

/// The five dispatches, in order.
enum class DofStep : u8 {
    Setup = 0,
    Tiles,
    Dilate,
    Gather,
    Composite,
    Count,
};

inline constexpr u32 kDofStepCount = static_cast<u32>(DofStep::Count);

/// What the last declared frame did.
struct DofReport {
    /// Passes declared by the last `declare`: five, or zero after a refusal.
    u32 passes = 0;
    /// Dispatches recorded since the report was reset.
    u32 dispatches = 0;
};

class DepthOfFieldPass {
public:
    DepthOfFieldPass() = default;
    ~DepthOfFieldPass();

    DepthOfFieldPass(const DepthOfFieldPass&) = delete;
    DepthOfFieldPass& operator=(const DepthOfFieldPass&) = delete;
    DepthOfFieldPass(DepthOfFieldPass&&) = delete;
    DepthOfFieldPass& operator=(DepthOfFieldPass&&) = delete;

    /// A compute stage and a native shader format this module ships: SPIR-V or MSL.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    [[nodiscard]] Status create(rhi::Device& device,
                                const DepthOfFieldPassDescription& desc) noexcept;
    /// Release everything. Idempotent; the caller waits for the device first.
    void destroy() noexcept;

    /// The lens and the gather's limits. Checked, with the view, by `set_view`.
    void set_settings(const DofSettings& settings) noexcept { settings_ = settings; }
    [[nodiscard]] const DofSettings& settings() const noexcept { return settings_; }
    /// This frame's projection. Refuses a view of another extent and settings `make_dof_constants`
    /// refuses.
    [[nodiscard]] Status set_view(const DofView& view) noexcept;
    [[nodiscard]] const DofConstants& constants() const noexcept { return constants_; }

    /// The frame's hook: the stage declared as this module's five passes.
    [[nodiscard]] FrameStageDeclaration stage() noexcept;
    /// Declare the five passes. Returns the first, or `kInvalidPass` when there is no view, or the
    /// inputs are missing or not the extent this pass was created for.
    [[nodiscard]] PassId declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept;

    [[nodiscard]] const DofReport& report() const noexcept { return report_; }
    void reset_report() noexcept { report_.dispatches = 0; }

    // Public because `RecordFn` is a plain function pointer: each pass's user pointer is one of
    // these, naming the pass and the dispatch it records.
    struct Step {
        DepthOfFieldPass* self = nullptr;
        DofStep step = DofStep::Count;
    };
    void record(DofStep step, const PassContext& context) noexcept;

private:
    [[nodiscard]] Status create_pipeline(DofStep step) noexcept;
    static PassId declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                void* user) noexcept;

    rhi::Device* device_ = nullptr;
    DepthOfFieldPassDescription desc_{};
    DofSettings settings_{};
    DofConstants constants_{};
    bool viewed_ = false;
    DofReport report_{};

    rhi::ShaderModuleHandle shaders_[kDofStepCount];
    rhi::DescriptorSetLayoutHandle set_layouts_[kDofStepCount];
    rhi::PipelineLayoutHandle pipeline_layouts_[kDofStepCount];
    rhi::ComputePipelineHandle pipelines_[kDofStepCount];
    Step steps_[kDofStepCount];

    // This frame's resources, from `declare`.
    ResourceId source_ = kInvalidResource;
    ResourceId depth_ = kInvalidResource;
    ResourceId layer_ = kInvalidResource;
    ResourceId tiles_ = kInvalidResource;
    ResourceId dilated_ = kInvalidResource;
    ResourceId far_ = kInvalidResource;
    ResourceId near_ = kInvalidResource;
    ResourceId target_ = kInvalidResource;
};

}  // namespace cy::rendering::depth_of_field
