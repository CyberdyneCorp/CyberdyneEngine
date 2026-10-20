// SPDX-License-Identifier: MIT
#pragma once
// The GI debug views on the device, drawn from the device frame's own GI state. Issue #35, the
// live half.
//
// ================================================================================================
// WHAT IT REPLACES, AND WHAT IT IS HELD TO
// ================================================================================================
//
// `gi::render_gi_debug_view` (gi/debug_views.h) draws each of the nineteen views on the processor,
// from a surface buffer its caller traces and from the host system's queries: about 35 ms for the
// probe view and 240 ms for indirect diffuse at 320x180, which is a diagnostic rather than a
// viewport. This class draws the same views on the device, in the frame they describe: its pixels
// are the resolve's surfaces (the prepass the frame drew), and its data the device copies the frame
// was lit from — the probe set, the cards and their lookup, the field, the published targets and
// the reprojection's history. Every function in gi_debug_views_common.slang names the host
// function it transcribes; `render.gi_gpu_live` draws every view both ways over one frame's
// surfaces and compares the words, and states each view's bound.
//
// The views the device frame holds a datum of its own for draw the device's (the shader's header
// lists them): indirect diffuse and specular are the published targets the forward pass
// composites, the denoiser views the reprojection's history, the screen-trace view red (the frame's
// rays have no screen tier). `GiReflectionProbes` is drawn only where the system has no reflection
// probes — the device holds none of their captures — and `draws()` says so, so a caller falls back
// to the host view there.
//
// ================================================================================================
// IN THE FRAME
// ================================================================================================
//
// Declared inside the frame's `ScreenSpaceGi` stage, after the resolve's publish
// (`GpuGiFrame` does it): an exposure pass, the view, the leak suspects' discs for
// `GiProbeVisibility`, and a copy into an `Rgba8Unorm` texture the forward pass draws where the
// view is opaque (`lightmaps::write_gi_debug_view`). The opaque pass reads `import_texture`'s
// resource, so the graph orders it after the copy.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/rendering/gi/debug_views.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/gi_gpu/gpu_resolve.h>
#include <cy/rendering/graph/graph.h>

namespace cy::rendering::gi_gpu {

struct GpuGiDebugViewDescription {
    /// The most promoted emitters the emissive view lights a surface with.
    u32 max_emitters = 64;
    /// A host copy of every view drawn, for a test.
    bool readback = false;
};

/// What one frame's view is drawn with, beyond the frame itself.
struct GpuGiDebugViewSettings {
    gi::GiDebugView view = gi::GiDebugView::Count;
    /// Zero is automatic: the multiplier that draws the frame's mean indirect diffuse at a third of
    /// white. `GiDebugInputs::exposure` otherwise.
    f32 exposure = 0.0F;
    /// `GiDebugInputs::field_slice_height`, `probe_disc_metres` and `max_age_frames`.
    f32 field_slice_height = 0.0F;
    f32 probe_disc_metres = 0.12F;
    u64 max_age_frames = 240;
};

class GpuGiDebugView {
public:
    GpuGiDebugView() noexcept = default;
    ~GpuGiDebugView();

    GpuGiDebugView(const GpuGiDebugView&) = delete;
    GpuGiDebugView& operator=(const GpuGiDebugView&) = delete;
    GpuGiDebugView(GpuGiDebugView&&) = delete;
    GpuGiDebugView& operator=(GpuGiDebugView&&) = delete;

    /// Create over `resolve`, which must be created; the views read its surfaces, its history, its
    /// published targets and, through its radiance cache, the probe set and the scene.
    [[nodiscard]] Status create(Allocator& allocator, rhi::Device& device,
                                GpuScreenResolve& resolve,
                                const GpuGiDebugViewDescription& desc) noexcept;
    void destroy() noexcept;
    [[nodiscard]] bool created() const noexcept { return device_ != nullptr; }

    /// Whether the device draws `view` for `system`: every view but `GiReflectionProbes` where the
    /// system holds reflection probes, whose captures the device does not have.
    [[nodiscard]] static bool draws(gi::GiDebugView view,
                                    const gi::IlluminationSystem& system) noexcept;

    /// This frame's view. After the resolve's `begin_frame`, before the frame's stage is declared.
    /// `GiDebugView::Count` draws none. Refused for a view `draws()` answers no for.
    [[nodiscard]] Status prepare(const gi::IlluminationSystem& system, Vec3 camera, u64 frame,
                                 const GpuGiDebugViewSettings& settings) noexcept;
    [[nodiscard]] bool active() const noexcept { return view_ != gi::GiDebugView::Count; }
    [[nodiscard]] gi::GiDebugView view() const noexcept { return view_; }

    /// The view's passes, after the resolve's. Nothing when no view is prepared.
    [[nodiscard]] Status declare(RenderGraph& graph) noexcept;
    /// The texture the forward pass samples, imported into `graph`: the opaque pass's read. Once a
    /// frame, after `prepare` and before the stage is declared, which writes this same resource.
    [[nodiscard]] ResourceId import_texture(RenderGraph& graph) noexcept;
    [[nodiscard]] rhi::TextureViewHandle texture_view() const noexcept { return texture_view_; }

    /// After the frame's fence, with `readback`: the view's words, one a frame pixel.
    [[nodiscard]] Status read_back(Span<u32> out) const noexcept;
    /// After the frame's fence, with `readback`: the exposure the view was drawn at.
    [[nodiscard]] Expected<f32, Error> read_back_exposure() const noexcept;

private:
    enum Pipeline : u32 { kExposure = 0, kView, kSuspects, kViewHardware, kPipelineCount };
    enum Buffer : u32 {
        kSuspectFlags = 0,
        kExposureWord,
        kOutput,
        kOutputHost,
        kExposureHost,
        kBufferCount,
    };
    struct Step {
        GpuGiDebugView* self = nullptr;
        Pipeline pipeline = kView;
        u32 groups_x = 1;
        u32 groups_y = 1;
    };
    struct Copy {
        GpuGiDebugView* self = nullptr;
        Buffer source = kOutput;
        Buffer destination = kOutputHost;
        u64 bytes = 0;
    };

    [[nodiscard]] Status create_layouts() noexcept;
    [[nodiscard]] Status create_pipelines() noexcept;
    [[nodiscard]] Status create_resources() noexcept;
    [[nodiscard]] Status write_emitters(const gi::IlluminationSystem& system) noexcept;
    [[nodiscard]] Expected<rhi::DescriptorSetHandle, Error> transient_set(
        const PassContext& context) noexcept;
    [[nodiscard]] static ResourceId import_buffer(RenderGraph& graph, rhi::BufferHandle buffer,
                                                  u64 size, const char* name) noexcept;
    void read_frame(RenderGraph& graph, PassBuilder& pass) noexcept;

    static void record_step(const PassContext& context, void* user) noexcept;
    static void record_texture_copy(const PassContext& context, void* user) noexcept;
    static void record_copy(const PassContext& context, void* user) noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    GpuScreenResolve* resolve_ = nullptr;
    GpuGiDebugViewDescription desc_{};
    gi::GiDebugView view_ = gi::GiDebugView::Count;
    bool hardware_ = false;

    rhi::DescriptorSetLayoutHandle layout_;
    rhi::PipelineLayoutHandle pipeline_layout_;
    rhi::PipelineLayoutHandle hardware_pipeline_layout_;
    rhi::ShaderModuleHandle shaders_[kPipelineCount];
    rhi::ComputePipelineHandle pipelines_[kPipelineCount];
    bool hardware_pipeline_ = false;

    /// The words and the emitters: a ring of `rhi::kMaxFramesInFlight`, as the resolve's words.
    rhi::BufferHandle words_[rhi::kMaxFramesInFlight];
    rhi::BufferHandle emitters_[rhi::kMaxFramesInFlight];
    u32 ring_ = 0;
    bool began_ = false;
    rhi::BufferHandle buffers_[kBufferCount];
    u64 sizes_[kBufferCount] = {};
    rhi::TextureHandle texture_;
    rhi::TextureViewHandle texture_view_;
    u32 width_ = 0;
    u32 height_ = 0;
    u32 probe_slots_ = 0;

    // This frame's graph resources, valid between `declare` and the graph's execution.
    ResourceId texture_resource_ = kInvalidResource;
    ResourceId diffuse_ = kInvalidResource;
    ResourceId specular_ = kInvalidResource;
    Step steps_[3]{};
    Copy copies_[2]{};
};

}  // namespace cy::rendering::gi_gpu
