// SPDX-License-Identifier: MIT
#pragma once
// The built-in temporal upscaler on the device: two dispatches, a resample, a copy for the reactive
// mask, and the histories they keep. `rendering-post-processing` — "Temporal upscaling".
//
// ================================================================================================
// THE SHAPE, AND WHERE IT SITS IN THE FRAME
// ================================================================================================
//
//   render extent                                        output extent
//   colour + depth + motion + pre-translucency ──► reconstruct ──► history (kept) ──► sharpen ──►
//   last frame's history and state ──────────────┘    └──────────► state (kept)       upscaled
//                                                                                     colour
//   depth + motion ──► resample ──► output depth + output motion      (only when a later stage
//                                                                       reads them)
//
// Declared at the frame's own `FramePassKind::Temporal` stage through `FrameStageDeclaration` —
// step 5 of the chain — so everything before it (shading, global illumination, fog, translucency)
// runs at the render extent and everything after it (depth of field, motion blur, bloom, exposure,
// tonemapping, the outlines and the interface) at the output extent.
//
// THE HISTORIES ARE THE PASS'S, AND THE TEMPORAL FRAMEWORK DECIDES WHEN THEY ARE VALID. Two output-
// extent RGBA16F colour histories — colour and the evidence it holds — and two output-extent RG32F
// state images — a texel's lock and the depth of the surface it holds — alternating, imported into
// each frame's graph in the use the last executed frame left them in. Both are at the OUTPUT
// extent, so dynamic resolution moving the render extent never reallocates or invalidates them.
// `bind_temporal` registers the pass as the framework's `post.temporal_upscaling` consumer and
// declares both, so a cut, a teleport or a projection change invalidates them in the frame it
// happens — the pass has no invalidation logic of its own. `executed()` (called by
// `FrameAssembly::execute`) swaps them and marks them written, so a frame that never ran never
// becomes history.
//
// ================================================================================================
// WHAT A CALLER DOES
// ================================================================================================
//
//     pass.create(allocator, device, {output extent});
//     pass.bind_temporal(assembly.temporal());
//     pass.set_settings(settings);                 // mode, sharpness, exposure
//     // per frame:
//     sinks.temporal = upscale::stage_of(pass);
//     sinks.opaque_copy = pass.opaque_copy_stage(); // with AssemblyDescription::reactive_mask

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/temporal/framework.h>
#include <cy/rendering/upscale/upscale.h>
#include <cy/rendering/upscale/upscaler.h>

namespace cy::rendering::upscale {

struct UpscalePassDescription {
    /// The output extent — and the largest extent the scene may be rendered at.
    Extent2D output;
    /// The formats of the frame's depth and motion targets, which the resample writes at the output
    /// extent for the stages after the upscaler.
    rhi::Format depth_format = rhi::Format::D32Sfloat;
    rhi::Format velocity_format = rhi::Format::Rg16Sfloat;
    /// Whether `read_back` may be called: host copies of what the reconstruction read and wrote,
    /// which a test compares against the host reference.
    bool readback = false;
};

/// What `read_back` fills. An empty span is not read; a non-empty one is the extent named.
struct UpscaleReadback {
    /// Render extent: the colour, depth, motion and pre-translucency colour the reconstruction
    /// read.
    Span<Vec4> color;
    Span<f32> depth;
    Span<Vec2> velocity;
    Span<Vec4> opaque;
    /// Output extent: the history and the state it read and the ones it wrote.
    Span<Vec4> previous_history;
    Span<Vec4> history;
    Span<Vec2> previous_state;
    Span<Vec2> state;
    /// Output extent: the sharpened colour, and the resampled depth and motion when the frame had
    /// them.
    Span<Vec4> output;
    Span<f32> output_depth;
    Span<Vec2> output_velocity;
};

/// What the pass did, for a diagnostic and a test.
struct UpscaleReport {
    u64 frames_declared = 0;
    u64 frames_executed = 0;
    /// Frames whose history the framework had invalidated, or that had none yet.
    u64 history_resets = 0;
    /// The render extent of the last declared frame, and how many times it changed.
    Extent2D render;
    u32 render_changes = 0;
    bool resampled = false;
    bool reactive = false;
};

class TemporalUpscalePass final : public Upscaler {
public:
    TemporalUpscalePass() = default;
    ~TemporalUpscalePass() override;

    TemporalUpscalePass(const TemporalUpscalePass&) = delete;
    TemporalUpscalePass& operator=(const TemporalUpscalePass&) = delete;
    TemporalUpscalePass(TemporalUpscalePass&&) = delete;
    TemporalUpscalePass& operator=(TemporalUpscalePass&&) = delete;

    /// Compute, and a native shader format this module ships: SPIR-V or MSL.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const UpscalePassDescription& desc) noexcept;
    void destroy() noexcept;

    /// Register with the temporal framework the frame's jitter and invalidation come from:
    /// a consumer that needs jitter, and the colour (output extent) and depth (render extent)
    /// histories. Once, after `create`.
    [[nodiscard]] Status bind_temporal(TemporalFramework& framework) noexcept;

    /// Takes effect at the next frame's declaration. The mode is the caller's to apply to the
    /// render extent (`FrameAssembly::set_render_extent`); the reconstruction reads the extents it
    /// is handed.
    void set_settings(const UpscaleSettings& settings) noexcept { settings_ = settings; }
    [[nodiscard]] const UpscaleSettings& settings() const noexcept { return settings_; }

    /// The frame's `OpaqueColorCopy` stage, declared as this module's copy dispatch.
    [[nodiscard]] FrameStageDeclaration opaque_copy_stage() noexcept;

    // --- Upscaler ---------------------------------------------------------------------------
    [[nodiscard]] UpscalerFamily family() const noexcept override {
        return UpscalerFamily::BuiltIn;
    }
    [[nodiscard]] const char* name() const noexcept override {
        return "built-in temporal upscaler";
    }
    [[nodiscard]] bool available(const rhi::Device* device) const noexcept override;
    /// Declare the stage. Returns the first pass, or `kInvalidPass` when the inputs are not the
    /// ones this pass was created for — no colour or depth, an output extent other than its own, a
    /// render extent larger than it — or it was never bound to a temporal framework.
    [[nodiscard]] PassId declare(RenderGraph& graph,
                                 const ScreenSpaceStageInputs& inputs) noexcept override;
    void executed() noexcept override;

    /// The constants the last declared frame was recorded with.
    [[nodiscard]] const UpscaleConstants& constants() const noexcept { return constants_; }
    [[nodiscard]] const UpscaleReport& report() const noexcept { return report_; }
    /// The histories this pass declared with the framework: the colour and the state.
    [[nodiscard]] HistoryId colour_history() const noexcept { return colour_history_; }
    [[nodiscard]] HistoryId state_history() const noexcept { return state_history_; }

    /// What the last executed frame read and wrote. Call after its fence.
    [[nodiscard]] Status read_back(const UpscaleReadback& out) const noexcept;

private:
    enum Stage : u32 { kReconstruct = 0, kSharpen, kCopy, kComputeStageCount };
    enum Copy : u32 {
        kCopyColor = 0,
        kCopyDepth,
        kCopyVelocity,
        kCopyOpaque,
        kCopyPreviousHistory,
        kCopyHistory,
        kCopyPreviousState,
        kCopyState,
        kCopyOutput,
        kCopyOutputDepth,
        kCopyOutputVelocity,
        kCopyCount
    };

    struct Step {
        TemporalUpscalePass* self = nullptr;
        Stage stage = kReconstruct;
        ResourceId color = kInvalidResource;
        ResourceId depth = kInvalidResource;
        ResourceId velocity = kInvalidResource;
        ResourceId opaque = kInvalidResource;
        ResourceId previous_history = kInvalidResource;
        ResourceId previous_state = kInvalidResource;
        ResourceId output = kInvalidResource;
        ResourceId state = kInvalidResource;
        u32 width = 0;
        u32 height = 0;
    };
    /// The kept images this frame imported: the history and the state, read and written.
    struct HistoryResources {
        ResourceId previous = kInvalidResource;
        ResourceId current = kInvalidResource;
        ResourceId previous_state = kInvalidResource;
        ResourceId state = kInvalidResource;
    };
    struct Resample {
        TemporalUpscalePass* self = nullptr;
        ResourceId depth = kInvalidResource;
        ResourceId velocity = kInvalidResource;
        ResourceId output_depth = kInvalidResource;
        ResourceId output_velocity = kInvalidResource;
    };
    struct Readback {
        const TemporalUpscalePass* self = nullptr;
        ResourceId source = kInvalidResource;
        Copy copy = kCopyColor;
        u32 width = 0;
        u32 height = 0;
    };

    [[nodiscard]] Status create_compute(Stage stage) noexcept;
    [[nodiscard]] Status create_resample() noexcept;
    [[nodiscard]] Status create_resources() noexcept;
    [[nodiscard]] Status prepare(const ScreenSpaceStageInputs& inputs) noexcept;
    void declare_resample(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept;
    void declare_readbacks(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                           const HistoryResources& kept) noexcept;

    static PassId declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                void* user) noexcept;
    static PassId declare_copy_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                     void* user) noexcept;
    static void record_step(const PassContext& context, void* user) noexcept;
    static void record_resample(const PassContext& context, void* user) noexcept;
    static void record_readback(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    TemporalFramework* framework_ = nullptr;
    UpscalePassDescription desc_{};
    UpscaleSettings settings_{};
    UpscaleConstants constants_{};
    SharpenConstants sharpen_{};
    UpscaleReport report_{};
    ConsumerId consumer_;
    HistoryId colour_history_;
    HistoryId state_history_;

    rhi::ShaderModuleHandle shaders_[kComputeStageCount];
    rhi::DescriptorSetLayoutHandle set_layouts_[kComputeStageCount];
    rhi::PipelineLayoutHandle pipeline_layouts_[kComputeStageCount];
    rhi::ComputePipelineHandle pipelines_[kComputeStageCount];
    rhi::ShaderModuleHandle resample_shaders_[2];
    rhi::DescriptorSetLayoutHandle resample_set_layout_;
    rhi::PipelineLayoutHandle resample_layout_;
    rhi::GraphicsPipelineHandle resample_pipeline_;

    /// The alternating histories: `read_` is the last executed frame's.
    rhi::TextureHandle histories_[2];
    rhi::TextureHandle states_[2];
    /// What each was LEFT IN by the last executed frame — what it is imported as next.
    rhi::ImageUse history_uses_[2] = {rhi::ImageUse::Undefined, rhi::ImageUse::Undefined};
    rhi::ImageUse state_uses_[2] = {rhi::ImageUse::Undefined, rhi::ImageUse::Undefined};
    /// What this frame will leave them in, committed by `executed()`.
    rhi::ImageUse pending_history_uses_[2] = {};
    rhi::ImageUse pending_state_uses_[2] = {};
    u32 read_ = 0;
    /// Whether a frame has executed since `create`.
    bool written_ = false;
    /// The render extent of the frame declared and not yet executed.
    Extent2D declared_render_{};
    bool declared_ = false;
    rhi::BufferHandle readbacks_[kCopyCount];
    /// What the last executed readback frame's buffers hold.
    Extent2D readback_render_{};
    Extent2D pending_readback_render_{};

    Step steps_[kComputeStageCount]{};
    Resample resample_{};
    Readback copies_[kCopyCount]{};
};

}  // namespace cy::rendering::upscale
