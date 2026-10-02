// SPDX-License-Identifier: MIT
#pragma once
// The runtime interface on the device: CyberUI's primitive stream drawn over the frame.
//
// ================================================================================================
// WHERE IT SITS IN THE FRAME
// ================================================================================================
//
//   post-process ─► output ─► selection outlines ─► ui (this) ─► present
//
// The frame's `FramePassKind::UiAndDebug` stage, declared through `FrameStageDeclaration`: after
// the tone curve, bloom, grading and the selection outlines, over the colour the chain ended in. A
// colour an interface asks for is therefore the colour in the output, byte for byte — `ui-system`'s
// "By default UI SHALL be drawn after tonemapping in display-referred colour". The HDR option the
// same requirement names, an interface composited before the tone curve, is not built.
//
// ================================================================================================
// WHAT A FRAME COSTS
// ================================================================================================
//
// One graph pass. Inside it, one `draw_indexed_indirect` per batch `flatten()` produced — a batch
// is one material, one atlas page and one clip — each an instanced quad per primitive, with the
// batch's scissor and its page's descriptor set. The primitive rows and the indirect arguments are
// host-visible buffers, one per frame in flight, written by `submit()` and read by the device; they
// are not graph resources because nothing on the device writes them.
//
// With nothing submitted — or with an empty stream — the pass is declared and records nothing, and
// the frame is the frame without an interface, byte for byte (`render.ui`).
//
// ================================================================================================
// WHAT A CALLER DOES
// ================================================================================================
//
//     renderer.create(device, {width, height, output_format});
//     renderer.upload_atlas(page, pixels...);           // outside a frame: a glyph or image page
//     // per frame:
//     flatten(store, viewport, buffer, report, &text);  // cy::ui
//     renderer.submit(buffer, scale);
//     sinks.ui = renderer.stage();                      // the assembly's FrameSinks

#include <cy/backends/rhi/device.h>
#include <cy/backends/rhi/handles.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/graph/graph.h>
#include <cy/ui/paint.h>
#include <cy/ui/render/encode.h>

namespace cy::ui::render {

/// The most atlas pages a renderer binds. Page zero is a single white texel, so a primitive that
/// names no page samples white.
inline constexpr u32 kMaxAtlasPages = 16;

struct UiRendererDescription {
    /// The target's extent: the frame's.
    u32 width = 0;
    u32 height = 0;
    /// The format the post chain ended in: the tonemapped output, normally `Rgba8Unorm`. An sRGB
    /// format sets `kUiOutputLinear`, so the bytes in the output are the interface's.
    rhi::Format output_format = rhi::Format::Rgba8Unorm;
    /// The most primitives one frame may submit. A larger stream is refused by `submit()` rather
    /// than truncated.
    u32 max_primitives = 16384;
};

/// What the last frame drew.
struct UiRenderReport {
    u32 primitives = 0;
    /// Indirect draws recorded: one per batch with a non-empty scissor.
    u32 draws = 0;
    /// Batches whose clip covered no pixel, and so drew nothing.
    u32 clipped_batches = 0;
    /// Descriptor sets bound: one per change of atlas page between consecutive draws.
    u32 page_binds = 0;
    /// Whether the pass recorded anything at all.
    bool recorded = false;
};

class UiRenderer {
public:
    explicit UiRenderer(Allocator& allocator) noexcept;
    ~UiRenderer();

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;
    UiRenderer(UiRenderer&&) = delete;
    UiRenderer& operator=(UiRenderer&&) = delete;

    /// A native shader format this module ships: SPIR-V or MSL.
    [[nodiscard]] static bool supported(const rhi::Device& device) noexcept;

    [[nodiscard]] Status create(rhi::Device& device, const UiRendererDescription& desc) noexcept;
    void destroy() noexcept;
    [[nodiscard]] bool ready() const noexcept { return device_ != nullptr; }

    /// Make `pixels` atlas page `page`: one byte a texel for `rhi::Format::R8Unorm` (glyph
    /// coverage), four for `Rgba8Unorm` (a premultiplied image, red in the low byte). Runs a graph
    /// of its own and waits for it, so call it OUTSIDE a frame; replaces whatever the page held.
    [[nodiscard]] Status upload_atlas(u16 page, rhi::Format format, u32 width, u32 height,
                                      Span<const u8> pixels) noexcept;
    [[nodiscard]] bool has_atlas(u16 page) const noexcept;

    /// This frame's interface: `buffer` flattened in reference units, drawn at `scale` pixels per
    /// unit — `ui::resolve_scale`'s answer. Writes the frame slot's rows and arguments; call once a
    /// frame, after the device's frame has begun and before the graph executes.
    [[nodiscard]] Status submit(const PrimitiveBuffer& buffer, f32 scale) noexcept;
    /// Draw nothing this frame.
    void clear() noexcept;

    /// The frame's hook for `FrameSinks::ui`.
    [[nodiscard]] rendering::FrameStageDeclaration stage() noexcept;
    /// Declare the pass. Returns it, or `kInvalidPass` when the inputs are not the ones this
    /// renderer was created for.
    [[nodiscard]] rendering::PassId declare(
        rendering::RenderGraph& graph, const rendering::ScreenSpaceStageInputs& inputs) noexcept;

    [[nodiscard]] const UiRenderReport& report() const noexcept { return report_; }
    /// The draws the last `submit()` built, for a suite that compares against `draw_reference`.
    [[nodiscard]] const UiDrawList& draws() const noexcept { return list_; }

    // Public because `RecordFn` is a plain function pointer and the callback is a free function.
    void record(const rendering::PassContext& context) noexcept;

private:
    struct AtlasPage {
        rhi::TextureHandle texture;
        rhi::TextureViewHandle view;
    };

    [[nodiscard]] Status create_pipeline() noexcept;
    [[nodiscard]] Status create_buffers() noexcept;
    void destroy_page(AtlasPage& page) noexcept;
    [[nodiscard]] Expected<rhi::DescriptorSetHandle, Error> set_for(u16 page, u32 slot) noexcept;
    static rendering::PassId declare_stage(rendering::RenderGraph& graph,
                                           const rendering::ScreenSpaceStageInputs& inputs,
                                           void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    UiRendererDescription desc_{};
    UiRenderReport report_{};
    UiDrawList list_;
    u32 flags_ = 0;

    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragment_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle layout_;
    rhi::GraphicsPipelineHandle pipeline_;
    rhi::SamplerHandle point_sampler_;
    rhi::SamplerHandle linear_sampler_;
    rhi::BufferHandle indices_;

    /// One per frame in flight: the rows and the indirect arguments `submit()` wrote.
    rhi::BufferHandle rows_[rhi::kMaxFramesInFlight];
    rhi::BufferHandle arguments_[rhi::kMaxFramesInFlight];
    u32 slots_ = 0;
    u32 slot_ = 0;

    AtlasPage pages_[kMaxAtlasPages];

    // This frame's target, from `declare`.
    rendering::ResourceId target_ = rendering::kInvalidResource;
};

}  // namespace cy::ui::render
