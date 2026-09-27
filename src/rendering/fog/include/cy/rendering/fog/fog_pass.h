// SPDX-License-Identifier: MIT
#pragma once
// Volumetric fog on the device: the march and the volume the frame reads.
// `rendering-post-processing` — "Volumetric fog".
//
// ================================================================================================
// THE SHAPE, AND WHERE IT SITS IN THE FRAME
// ================================================================================================
//
//   shadow map ──► march every froxel column ──► volume ──► opaque (surfaces through the fog)
//
// One compute pass, declared at the frame's own `FramePassKind::VolumetricFog` stage through
// `FrameStageDeclaration` — after the shadow pass that writes the map and before the opaque pass
// that samples the volume.
//
// THE VOLUME IS PERSISTENT AND IMPORTED for the reason `contact_shadows::ContactShadowPass` gives:
// the forward pass reads it through a slot of the frame's set 0 texture table, and a slot names a
// view that must outlive the graph. It is imported `Undefined` each frame because the march writes
// every texel a consumer reads.
//
// THE SHADOW MAP IS THE CALLER'S, named by the resource it imported for this frame and the view the
// march binds. A frame without one marches unshadowed and binds a one-texel placeholder the shader
// never reads.
//
// ================================================================================================
// WHAT A CALLER DOES
// ================================================================================================
//
//     pass.create(allocator, device, {settings});
//     pass.set_medium(medium);
//     // per frame, before `FrameAssembly::assemble`, with the post chain's `volumetric_fog` on:
//     pass.set_frame({view, light, shadow, shadow_resource});
//     view.volumetric_fog = pass.import_target(graph);
//     sinks.volumetric_fog = pass.stage();
//     // and the consumer names `pass.target_view()` at a slot of its texture table.
//
// OR THE ATMOSPHERE'S TABLE. Created with `FogTarget::AerialTable`, the volume is a buffer in
// `sky::pack_aerial_perspective`'s layout instead, and `FogFrame::air` names the atmosphere's own
// table for the march to composite with. A consumer that binds an aerial perspective table binds
// `target_buffer()` in its place and applies both media through `cy/aerial_perspective.slang`.

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/backends/rhi/types.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/fog/medium.h>
#include <cy/rendering/fog/volume.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/graph/graph.h>

namespace cy::rendering::fog {

/// What the march writes.
enum class FogTarget : u8 {
    /// `cy/volumetric_fog.slang`'s texture, read through a slot of the frame's texture table.
    Texture = 0,
    /// `cy/aerial_perspective.slang`'s buffer, the atmosphere composited in.
    AerialTable,
};

struct FogPassDescription {
    FogSettings settings{};
    FogTarget target = FogTarget::Texture;
    /// Whether `read_back` may be called: a host copy of the volume, which a test reads.
    bool readback = false;
};

/// One frame's camera, light and shadow map.
struct FogFrame {
    FogView view{};
    FogLight light{};
    FogShadow shadow{};
    /// The shadow map as this frame's graph imported it. Read only when `shadow.enabled`, through
    /// the view the graph realises for the march's declared read.
    ResourceId shadow_resource = kInvalidResource;
    /// `FogTarget::AerialTable` only: the atmosphere's table for this camera, as this frame's graph
    /// imported it, or `kInvalidResource` to write the fog alone. Its words must describe the same
    /// camera the view does.
    ResourceId air = kInvalidResource;
};

class FogPass {
public:
    FogPass() = default;
    ~FogPass();

    FogPass(const FogPass&) = delete;
    FogPass& operator=(const FogPass&) = delete;
    FogPass(FogPass&&) = delete;
    FogPass& operator=(FogPass&&) = delete;

    /// Compute, and a native shader format this module ships: SPIR-V or MSL.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                const FogPassDescription& desc) noexcept;
    void destroy() noexcept;

    void set_medium(const FogMedium& medium) noexcept { medium_ = medium; }
    [[nodiscard]] const FogMedium& medium() const noexcept { return medium_; }
    [[nodiscard]] const FogSettings& settings() const noexcept { return desc_.settings; }

    /// This frame's camera, light and shadow map. Packs the constants the march reads.
    [[nodiscard]] Status set_frame(const FogFrame& frame) noexcept;
    [[nodiscard]] const FogConstants& constants() const noexcept { return constants_; }

    /// Import the persistent volume into this frame's graph, before the frame is declared.
    [[nodiscard]] ResourceId import_target(RenderGraph& graph) noexcept;

    /// The frame's hook: the stage declared as this module's pass.
    [[nodiscard]] FrameStageDeclaration stage() noexcept;

    /// Declare the march. Returns its pass, or `kInvalidPass` when the inputs are not the ones this
    /// pass was prepared for.
    [[nodiscard]] PassId declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept;

    /// The volume, `fog_texture_extent(settings().volume)` texels of `target_format()`. Null for
    /// `FogTarget::AerialTable`.
    [[nodiscard]] rhi::TextureViewHandle target_view() const noexcept { return target_view_; }
    /// `FogTarget::AerialTable`'s buffer, `fog_table_words(settings().volume)` float4 words. Null
    /// for `FogTarget::Texture`.
    [[nodiscard]] rhi::BufferHandle target_buffer() const noexcept { return table_; }
    [[nodiscard]] FogTarget target() const noexcept { return desc_.target; }
    [[nodiscard]] static constexpr rhi::Format target_format() noexcept {
        return rhi::Format::Rgba32Sfloat;
    }
    [[nodiscard]] FogTextureExtent extent() const noexcept {
        return fog_texture_extent(desc_.settings.volume);
    }

    /// The volume from the last executed frame: the texture's texels row-major from row 0, or the
    /// table's words. Call after its fence.
    [[nodiscard]] Status read_back(Span<Vec4> out) const noexcept;
    /// How many `Vec4` `read_back` writes.
    [[nodiscard]] usize readback_count() const noexcept;

private:
    struct March {
        FogPass* self = nullptr;
        ResourceId shadow = kInvalidResource;
        ResourceId output = kInvalidResource;
        ResourceId air = kInvalidResource;
    };
    struct Readback {
        const FogPass* self = nullptr;
        ResourceId source = kInvalidResource;
    };

    [[nodiscard]] Status create_pipeline() noexcept;
    [[nodiscard]] Status create_resources() noexcept;

    static PassId declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                void* user) noexcept;
    static void record_march(const PassContext& context, void* user) noexcept;
    static void record_readback(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    FogPassDescription desc_{};
    FogMedium medium_{};
    FogFrame frame_{};
    FogConstants constants_{};
    bool framed_ = false;

    rhi::ShaderModuleHandle shader_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle pipeline_layout_;
    rhi::ComputePipelineHandle pipeline_;

    rhi::TextureHandle target_;
    rhi::TextureViewHandle target_view_;
    /// The shadow map a frame without one binds: one texel, never read.
    rhi::TextureHandle placeholder_;
    /// `FogTarget::AerialTable`: the output, and the atmosphere a frame without one binds — its
    /// header alone, never read.
    rhi::BufferHandle table_;
    rhi::BufferHandle no_air_;
    /// One constant block per frame in flight, written when its frame records.
    rhi::BufferHandle words_[rhi::kMaxFramesInFlight] = {};
    rhi::BufferHandle readback_;

    ResourceId imported_ = kInvalidResource;
    March march_{};
    Readback copy_{};
};

}  // namespace cy::rendering::fog
