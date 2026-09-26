// SPDX-License-Identifier: MIT
#pragma once
// Contact shadows on the device: the trace and the target the forward pass reads.
// `virtual-shadows` — "Contact and traced refinement".
//
// ================================================================================================
// THE SHAPE, AND WHERE IT SITS IN THE FRAME
// ================================================================================================
//
//   depth + normal (prepass) ──► trace toward the light ──► target ──► opaque (sun visibility)
//
// One compute pass, declared at the frame's own `FramePassKind::ContactShadows` stage through
// `FrameStageDeclaration` — after the prepass and before the opaque pass that samples the target.
//
// THE TARGET IS PERSISTENT AND IMPORTED for the reason `occlusion::AmbientOcclusionPass` gives: the
// forward pass reads it through a slot of the frame's set 0 texture table, and a slot names a view
// that must outlive the graph. It is imported `Undefined` each frame because the trace writes every
// texel.
//
// ================================================================================================
// WHAT A CALLER DOES
// ================================================================================================
//
//     pass.create(allocator, device, {width, height});
//     // per frame, before `FrameAssembly::assemble`, with `AssemblyDescription::contact_shadows`:
//     pass.set_view({projection, relative_to_view, to_light, width, height});
//     view.contact_shadows = pass.import_target(graph);
//     sinks.contact_shadows = pass.stage();
//     // and the consumer names `pass.target_view()` at a slot of its texture table, and sets
//     // `kSoftShadowContact` with that slot in `FrameViewData::soft_shadow_control`.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/contact_shadows/contact.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/graph/graph.h>

namespace cy::rendering::contact_shadows {

struct ContactShadowPassDescription {
    /// The frame's extent. The term is full resolution.
    u32 width = 0;
    u32 height = 0;
    /// Whether `read_back` may be called: a host copy of the target, which a test reads.
    bool readback = false;
};

class ContactShadowPass {
public:
    ContactShadowPass() = default;
    ~ContactShadowPass();

    ContactShadowPass(const ContactShadowPass&) = delete;
    ContactShadowPass& operator=(const ContactShadowPass&) = delete;
    ContactShadowPass(ContactShadowPass&&) = delete;
    ContactShadowPass& operator=(ContactShadowPass&&) = delete;

    /// Compute, and a native shader format this module ships: SPIR-V or MSL.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const ContactShadowPassDescription& desc) noexcept;
    void destroy() noexcept;

    void set_settings(const ContactShadowSettings& settings) noexcept { settings_ = settings; }
    [[nodiscard]] const ContactShadowSettings& settings() const noexcept { return settings_; }

    /// This frame's projection, camera rotation, light and jitter. Must match the prepass that
    /// wrote the depth.
    [[nodiscard]] Status set_view(const ContactShadowView& view) noexcept;
    [[nodiscard]] const ContactShadowConstants& constants() const noexcept { return constants_; }

    /// Import the persistent target into this frame's graph, before the frame is declared.
    [[nodiscard]] ResourceId import_target(RenderGraph& graph) noexcept;

    /// The frame's hook: the stage declared as this module's pass.
    [[nodiscard]] FrameStageDeclaration stage() noexcept;

    /// Declare the trace. Returns its pass, or `kInvalidPass` when the inputs are not the ones this
    /// pass was prepared for.
    [[nodiscard]] PassId declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept;

    /// The term: visibility in `.r`, 1 lit and 0 in contact shadow.
    [[nodiscard]] rhi::TextureViewHandle target_view() const noexcept { return target_view_; }
    [[nodiscard]] static constexpr rhi::Format target_format() noexcept {
        return rhi::Format::Rgba8Unorm;
    }

    /// The visibility from the last executed frame, `width * height`, row-major from the top-left.
    /// Call after its fence.
    [[nodiscard]] Status read_back(Span<f32> out) const noexcept;

private:
    struct Trace {
        ContactShadowPass* self = nullptr;
        ResourceId depth = kInvalidResource;
        ResourceId normals = kInvalidResource;
        ResourceId output = kInvalidResource;
    };
    struct Readback {
        const ContactShadowPass* self = nullptr;
        ResourceId source = kInvalidResource;
    };

    [[nodiscard]] Status create_pipeline() noexcept;
    [[nodiscard]] Status create_resources() noexcept;

    static PassId declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                void* user) noexcept;
    static void record_trace(const PassContext& context, void* user) noexcept;
    static void record_readback(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    ContactShadowPassDescription desc_{};
    ContactShadowSettings settings_{};
    ContactShadowView view_{};
    ContactShadowConstants constants_{};

    rhi::ShaderModuleHandle shader_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle pipeline_layout_;
    rhi::ComputePipelineHandle pipeline_;

    rhi::TextureHandle target_;
    rhi::TextureViewHandle target_view_;
    rhi::BufferHandle readback_;

    ResourceId imported_ = kInvalidResource;
    Trace trace_{};
    Readback copy_{};
};

}  // namespace cy::rendering::contact_shadows
