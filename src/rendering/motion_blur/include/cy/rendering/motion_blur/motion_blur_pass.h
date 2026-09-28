// SPDX-License-Identifier: MIT
#pragma once
// Motion blur on the device: three dispatches and the target the post chain continues from.
// `rendering-post-processing` — "Motion blur".
//
// ================================================================================================
// THE SHAPE, AND WHERE IT SITS IN THE FRAME
// ================================================================================================
//
//   velocity + depth ──► tile max ──► neighbour max ─┐
//   temporal colour + velocity + depth ──────────────┴─► gather ──► target ──► bloom / exposure
//
// Declared at the frame's own `FramePassKind::MotionBlur` stage through `FrameStageDeclaration` —
// step 8 of the chain, after the temporal resolve and before bloom — so the streak of a bright
// light blooms as the light it is, and the temporal history the resolve keeps is never blurred.
//
// THE TARGET IS PERSISTENT AND IMPORTED, `Undefined` every frame because the gather writes every
// texel, for the reason `occlusion::AmbientOcclusionPass` gives: a producer's target is the
// producer's. The two tile images are graph transients, because nothing outside this frame reads
// them.
//
// ================================================================================================
// WHAT A CALLER DOES
// ================================================================================================
//
//     pass.create(allocator, device, {width, height});
//     // per frame, before `FrameAssembly::assemble`, with `post.motion_blur`:
//     pass.set_settings(settings_for_camera(settings, camera.shutter_seconds, frame_seconds));
//     pass.set_view({width, height, projection, relative_to_clip, previous_relative_to_clip});
//     view.motion_blur = pass.declare_target(graph);
//     sinks.motion_blur = pass.stage();

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/motion_blur/motion_blur.h>

namespace cy::rendering::motion_blur {

struct MotionBlurPassDescription {
    /// The frame's extent. The blur is full resolution.
    u32 width = 0;
    u32 height = 0;
    /// Whether `read_back` may be called: host copies of what the gather read and wrote, which a
    /// test compares against the host reference.
    bool readback = false;
};

/// What `read_back` fills. An empty span is not read.
struct MotionBlurReadback {
    /// The colour the gather read — the temporal resolve's output.
    Span<Vec4> input;
    Span<Vec2> velocity;
    Span<f32> depth;
    /// The blurred colour.
    Span<Vec4> output;
};

class MotionBlurPass {
public:
    MotionBlurPass() = default;
    ~MotionBlurPass();

    MotionBlurPass(const MotionBlurPass&) = delete;
    MotionBlurPass& operator=(const MotionBlurPass&) = delete;
    MotionBlurPass(MotionBlurPass&&) = delete;
    MotionBlurPass& operator=(MotionBlurPass&&) = delete;

    /// Compute, and a native shader format this module ships: SPIR-V or MSL.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const MotionBlurPassDescription& desc) noexcept;
    void destroy() noexcept;

    /// Takes effect at the next `set_view`.
    void set_settings(const MotionBlurSettings& settings) noexcept { settings_ = settings; }
    [[nodiscard]] const MotionBlurSettings& settings() const noexcept { return settings_; }

    /// This frame's projection and camera motion. Must match the prepass that wrote the velocity.
    [[nodiscard]] Status set_view(const MotionBlurView& view) noexcept;
    [[nodiscard]] const MotionBlurConstants& constants() const noexcept { return constants_; }

    /// Declare this frame's target in its graph, before the frame is declared.
    ///
    /// A FRAME TRANSIENT, NOT AN IMPORTED IMAGE. Nothing reads the blur after the frame, and a
    /// persistent target imported into the graph did not reach the post-process: sampled there
    /// through the executor's view it read as the temporal history it was blurred from, while a
    /// copy out of it held the blur (the module README records the measurement).
    [[nodiscard]] ResourceId declare_target(RenderGraph& graph) noexcept;

    /// The frame's hook: the stage declared as this module's three passes.
    [[nodiscard]] FrameStageDeclaration stage() noexcept;

    /// Declare the three dispatches. Returns the first, or `kInvalidPass` when the inputs are not
    /// the ones this pass was prepared for — no velocity, no colour, another extent.
    [[nodiscard]] PassId declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept;

    [[nodiscard]] static constexpr rhi::Format target_format() noexcept {
        return rhi::Format::Rgba16Sfloat;
    }

    /// What the last executed frame read and wrote, `width * height`, row-major from the top-left.
    /// Call after its fence.
    [[nodiscard]] Status read_back(const MotionBlurReadback& out) const noexcept;

private:
    /// `kCopy` is not part of the blur: a pass created with `readback` copies the gather's colour
    /// input into a texture of its own with it, so the readback never transfers out of the temporal
    /// history (motion_blur_copy.slang says why).
    enum Stage : u32 { kTileMax = 0, kNeighbourMax, kGather, kCopy, kStageCount };
    enum Copy : u32 { kCopyInput = 0, kCopyVelocity, kCopyDepth, kCopyOutput, kCopyCount };

    struct Step {
        MotionBlurPass* self = nullptr;
        Stage stage = kTileMax;
        ResourceId color = kInvalidResource;
        ResourceId velocity = kInvalidResource;
        ResourceId depth = kInvalidResource;
        ResourceId tiles = kInvalidResource;
        ResourceId neighbours = kInvalidResource;
        ResourceId output = kInvalidResource;
        ResourceId copy = kInvalidResource;
    };
    struct Readback {
        const MotionBlurPass* self = nullptr;
        ResourceId source = kInvalidResource;
        Copy copy = kCopyInput;
    };

    [[nodiscard]] Status create_pipeline(Stage stage) noexcept;
    [[nodiscard]] Status create_resources() noexcept;
    void declare_readbacks(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept;

    static PassId declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                void* user) noexcept;
    static void record_step(const PassContext& context, void* user) noexcept;
    static void record_readback(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    MotionBlurPassDescription desc_{};
    MotionBlurSettings settings_{};
    MotionBlurView view_{};
    MotionBlurConstants constants_{};

    rhi::ShaderModuleHandle shaders_[kStageCount];
    rhi::DescriptorSetLayoutHandle set_layouts_[kStageCount];
    rhi::PipelineLayoutHandle pipeline_layouts_[kStageCount];
    rhi::ComputePipelineHandle pipelines_[kStageCount];

    rhi::BufferHandle readbacks_[kCopyCount];

    ResourceId target_ = kInvalidResource;
    Step steps_[kStageCount]{};
    Readback copies_[kCopyCount]{};
};

}  // namespace cy::rendering::motion_blur
