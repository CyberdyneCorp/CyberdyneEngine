#pragma once
// The device side: pipelines, persistent resources, and the frame declared into the render graph.
//
// ================================================================================================
// WHAT A READER SHOULD TAKE AWAY FROM THIS FILE
// ================================================================================================
//
// It declares four passes and writes no barrier, because there is no way to write one: a pass gets
// a `cy::rhi::CommandBuffer`, and a command buffer has draws, dispatches, copies and debug labels
// on it and no synchronisation primitive at all (`rhi-and-render-graph`, and design.md §2). Every
// transition this frame needs — the shadow map into and out of its attachment layout, the colour
// target into transfer-source, the texture upload's two, the transfer-to-host at the end — is
// derived by the graph from the `read`/`write` lines in `Renderer::render`. That is the whole of
// the M3 invariant, seen from where a renderer author sits.
//
// ================================================================================================
// IT NAMES NO BACKEND
// ================================================================================================
//
// There is no Vulkan in this file and no `#if CY_RENDERER_VULKAN` either: it takes a
// `cy::rhi::Device&` and works against whichever one the host created. That is what lets
// `render.golden` photograph this frame on a device and `render.null_frame` compile the identical
// frame on a machine with no GPU — one renderer, two backends, and no branch between them
// (design.md §1).
//
// ================================================================================================
// ONE FRAME AT A TIME, DELIBERATELY
// ================================================================================================
//
// `render()` waits for the device before it returns. Frames in flight, per-frame descriptor pools
// and a ring of uniform buffers are all in the RHI and all exercised by the graph's own suites; a
// sample that used them would be teaching two things at once, and the second one would be
// bookkeeping. The cost is stated here rather than discovered: this sample is not a throughput
// demonstration.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>

#include "scene.h"

namespace cy::sample::first_light {

/// What the shadow map costs. 1024 texels over a 22-metre bounding sphere is about two centimetres
/// a texel, which is what the sample's normal offset and depth bias are chosen against.
inline constexpr u32 kShadowMapExtent = 1024;

/// The size of the per-draw push block, in bytes. Published because it is a claim rather than a
/// detail: what the frame pushes per draw is the OBJECT's transform and colour, and nothing else —
/// in particular not the view-projection, which travels in the descriptor-bound uniform buffer and
/// can therefore be replaced after the commands are recorded. `render.xr_prerequisites` asserts on
/// this number for exactly that reason, and renderer.cpp pins it to the struct.
inline constexpr u32 kObjectPushBytes = 64;

struct RendererOptions {
    u32 width = 192;
    u32 height = 108;
    /// Copy the colour target back into host memory at the end of the frame. On by default because
    /// this sample has no window to present to — see README.md, "Why there is no window".
    bool readback = true;
    /// Transient memory aliasing, so `--no-aliasing` can show what the frame's targets would cost
    /// without it. A policy knob, not a defect switch.
    bool aliasing = true;
};

/// What one frame did. Every field comes from the compiled plan or from the graph's execution
/// result, so the numbers a run prints are the graph's own rather than the renderer's guesses.
struct FrameReport {
    u32 submits = 0;
    u32 passes_recorded = 0;
    u32 passes_culled = 0;
    u32 barriers = 0;
    u32 barrier_batches = 0;
    u32 queue_ownership_transfers = 0;
    u64 transient_bytes = 0;
    u64 transient_bytes_without_aliasing = 0;
    u64 plan_hash = 0;
    u32 draws = 0;
    u32 triangles = 0;
};

/// The renderer. Holds the device's objects for the whole run and re-declares the frame each time.
class Renderer {
public:
    Renderer(Allocator& allocator, rhi::Device& device) noexcept;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Create the pipelines and the resources the scene needs. Separate from the constructor so
    /// that a caller which could not get a device does no work at all.
    [[nodiscard]] Status prepare(const Scene& scene, const RendererOptions& options) noexcept;

    /// Declare, compile and run one frame.
    [[nodiscard]] Expected<FrameReport, Error> render(const Scene& scene,
                                                      const Camera& camera) noexcept;

    /// The colour target of the last frame, as Rgba8Unorm texels, row-major from the top-left —
    /// which is the layout an image copy produces and the layout a PNG or a PPM wants. Empty when
    /// `RendererOptions::readback` is off, and empty on a backend that executes nothing.
    [[nodiscard]] Span<const u32> color_texels() const noexcept { return readback_.span(); }

    [[nodiscard]] u32 width() const noexcept { return options_.width; }
    [[nodiscard]] u32 height() const noexcept { return options_.height; }

    /// What the record callbacks are handed. Public because `RecordFn` is a plain function pointer,
    /// so the callbacks are free functions and cannot see a private nested type.
    struct PassState;

private:
    Status create_shaders() noexcept;
    Status create_pipelines() noexcept;
    Status create_resources(const Scene& scene) noexcept;
    Status upload_geometry(const Scene& scene) noexcept;
    /// Fill the per-frame uniform block from the camera and the sun.
    void write_frame_constants(const Scene& scene, const Camera& camera) noexcept;
    Status read_back_color() noexcept;

    Allocator* allocator_ = nullptr;
    rhi::Device* device_ = nullptr;
    RendererOptions options_{};

    rhi::ShaderModuleHandle shadow_vertex_;
    rhi::ShaderModuleHandle forward_vertex_;
    rhi::ShaderModuleHandle forward_fragment_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle pipeline_layout_;
    rhi::GraphicsPipelineHandle shadow_pipeline_;
    rhi::GraphicsPipelineHandle forward_pipeline_;
    rhi::DescriptorSetHandle descriptor_set_;

    rhi::BufferHandle vertices_;
    rhi::BufferHandle indices_;
    rhi::BufferHandle constants_;
    rhi::BufferHandle checker_staging_;
    rhi::BufferHandle readback_buffer_;
    rhi::TextureHandle albedo_;
    rhi::TextureViewHandle albedo_view_;
    /// A persistent texture rather than a graph transient, so that the descriptor set can be
    /// written once at start-up. Its layout is threaded across frames by `albedo_layout_` and
    /// `shadow_layout_` below and handed to `import_texture` each frame — getting that wrong is how
    /// a first barrier transitions from the wrong state, which is why the graph asks rather than
    /// assumes.
    rhi::TextureHandle shadow_map_;
    rhi::TextureViewHandle shadow_view_;
    rhi::SamplerHandle albedo_sampler_;
    rhi::SamplerHandle shadow_sampler_;

    rhi::ImageLayout albedo_layout_ = rhi::ImageLayout::Undefined;
    rhi::ImageLayout shadow_layout_ = rhi::ImageLayout::Undefined;
    /// The checkerboard is copied into the albedo texture on the first frame, by a pass the graph
    /// derives the two transitions around. Afterwards the pass is not declared at all — which is
    /// the same mechanism `rendering-forward-clustered` uses for a disabled feature.
    bool albedo_uploaded_ = false;

    Array<u32> readback_;
    u32 index_count_ = 0;
};

}  // namespace cy::sample::first_light
