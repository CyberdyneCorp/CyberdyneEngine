// SPDX-License-Identifier: MIT
#pragma once
// Ambient occlusion on the device: the horizon search, the shared denoiser's cascade, and the
// target the forward pass reads. `rendering-post-processing` — "Ambient occlusion".
//
// ================================================================================================
// THE SHAPE, AND WHERE IT SITS IN THE FRAME
// ================================================================================================
//
//   depth + normal (prepass) ──► horizons (raw) ──► filter, step 1 ──► step 2 ──► step 4 ──► target
//
// Five compute passes, declared at the frame's own `FramePassKind::AmbientOcclusion` stage through
// `FrameStageDeclaration` — after the prepass and before the opaque pass that samples the target.
// The barriers between them are the graph's; there is not one in this module.
//
// THE TARGET IS PERSISTENT AND IMPORTED, the intermediates are transients. The forward pass reads
// the term through a slot of the frame's set 0 texture table, and a slot names a VIEW: a
// transient's view is recreated every frame, so the one resource a consumer names outside the
// graph is the one this module owns. It is imported as `Undefined` each frame because the last
// filter pass rewrites every texel of it.
//
// ================================================================================================
// WHAT A CALLER DOES
// ================================================================================================
//
//     pass.create(allocator, device, {width, height});
//     // per frame, before `FrameAssembly::assemble`:
//     pass.set_view({projection, relative_to_view, width, height});
//     view.ambient_occlusion = pass.import_target(graph);
//     sinks.ambient_occlusion = pass.stage();
//     // and the consumer names `pass.target_view()` at a slot of its texture table.
//
// The post chain's `PostChainConfig::ambient_occlusion` is THE SETTING: off, the frame declares no
// stage, `stage()` is never called, and nothing here runs.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/occlusion/gtao.h>

namespace cy::rendering::occlusion {

struct AmbientOcclusionPassDescription {
    /// The frame's extent. The term is full resolution; see gtao.h.
    u32 width = 0;
    u32 height = 0;
    /// Whether `read_back` may be called: a host copy of the target the size of the view, which a
    /// shipped frame never reads and a test does.
    bool readback = false;
};

class AmbientOcclusionPass {
public:
    /// The horizon search and the filter; `kMaxFilterPasses` bounds the cascade the denoiser's
    /// configuration may ask for.
    static constexpr u32 kMaxFilterPasses = 4;

    AmbientOcclusionPass() = default;
    ~AmbientOcclusionPass();

    AmbientOcclusionPass(const AmbientOcclusionPass&) = delete;
    AmbientOcclusionPass& operator=(const AmbientOcclusionPass&) = delete;
    AmbientOcclusionPass(AmbientOcclusionPass&&) = delete;
    AmbientOcclusionPass& operator=(AmbientOcclusionPass&&) = delete;

    /// Compute, and a native shader format this module ships: SPIR-V or MSL. D3D12 has neither
    /// yet — see the README.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const AmbientOcclusionPassDescription& desc) noexcept;
    void destroy() noexcept;

    void set_settings(const GtaoSettings& settings) noexcept { settings_ = settings; }
    [[nodiscard]] const GtaoSettings& settings() const noexcept { return settings_; }

    /// This frame's projection and camera rotation. Must match the prepass that wrote the depth.
    [[nodiscard]] Status set_view(const GtaoView& view) noexcept;

    /// Import the persistent target into this frame's graph. Call once per frame, before the frame
    /// is declared; the id is what `AssemblyView::ambient_occlusion` carries.
    [[nodiscard]] ResourceId import_target(RenderGraph& graph) noexcept;

    /// The frame's hook: the stage declared as this module's passes.
    [[nodiscard]] FrameStageDeclaration stage() noexcept;

    /// Declare the horizon search and the cascade. Returns the first pass, or `kInvalidPass` when
    /// the inputs are not the ones this pass was prepared for — the target this frame imported, a
    /// depth and a normal buffer, the extent it was created at.
    [[nodiscard]] PassId declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept;

    /// The term: rgb the bent normal (camera-relative, normalise to read), a the visibility.
    [[nodiscard]] rhi::TextureHandle target() const noexcept { return target_; }
    [[nodiscard]] rhi::TextureViewHandle target_view() const noexcept { return target_view_; }
    [[nodiscard]] static constexpr rhi::Format target_format() noexcept {
        return rhi::Format::Rgba16Sfloat;
    }

    /// Copy of the target from the last executed frame. Call after its fence. `out` is `width *
    /// height`, row-major from the top-left.
    [[nodiscard]] Status read_back(Span<Vec4> out) const noexcept;
    /// The horizon search's own output from the same frame — the denoiser's input.
    [[nodiscard]] Status read_back_raw(Span<Vec4> out) const noexcept;

    /// How many passes the last `declare` added: one search, the cascade, and the readback copies.
    [[nodiscard]] u32 declared_passes() const noexcept { return declared_passes_; }

private:
    struct Step {
        AmbientOcclusionPass* self = nullptr;
        ResourceId depth = kInvalidResource;
        ResourceId normals = kInvalidResource;
        ResourceId raw = kInvalidResource;
        ResourceId source = kInvalidResource;
        ResourceId output = kInvalidResource;
        GtaoFilterConstants filter{};
    };
    struct Readback {
        const AmbientOcclusionPass* self = nullptr;
        ResourceId source = kInvalidResource;
        rhi::BufferHandle buffer;
    };

    [[nodiscard]] Status create_pipelines() noexcept;
    [[nodiscard]] Status create_pipeline(const char* name, bool filter) noexcept;
    [[nodiscard]] Status create_resources() noexcept;
    void declare_readback(RenderGraph& graph, Readback& readback, ResourceId source,
                          const char* name) noexcept;
    [[nodiscard]] Status copy_back(rhi::BufferHandle buffer, Span<Vec4> out) const noexcept;

    static PassId declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                void* user) noexcept;
    static void record_search(const PassContext& context, void* user) noexcept;
    static void record_filter(const PassContext& context, void* user) noexcept;
    static void record_readback(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    AmbientOcclusionPassDescription desc_{};
    GtaoSettings settings_{};
    GtaoView view_{};
    GtaoConstants constants_{};

    rhi::ShaderModuleHandle shaders_[2];
    rhi::DescriptorSetLayoutHandle set_layouts_[2];
    rhi::PipelineLayoutHandle pipeline_layouts_[2];
    rhi::ComputePipelineHandle pipelines_[2];

    rhi::TextureHandle target_;
    rhi::TextureViewHandle target_view_;
    rhi::BufferHandle readback_;
    rhi::BufferHandle raw_readback_;

    ResourceId imported_ = kInvalidResource;
    Step search_{};
    Step filters_[kMaxFilterPasses] = {};
    Readback readbacks_[2] = {};
    u32 declared_passes_ = 0;
};

}  // namespace cy::rendering::occlusion
