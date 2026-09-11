#pragma once
// THE FIRST CONSUMER OF THE PIPELINE LAYER, AND THE PROOF OF IT. M8.c task 1b.4.
//
// ================================================================================================
// IT PROVES THE LAYER BY USING IT, NOT BY A TEST WRITTEN BESIDE IT
// ================================================================================================
//
// Task 1b.4's whole point: a layer with only its own suite behind it is a layer nobody has had to
// live with. So this module writes NO frame, declares NO pass and creates NO descriptor set layout
// for anything the frame already has. It:
//
//   * reuses `FramePipelines`' set layouts 0 and 1 verbatim, so its pipeline layout is COMPATIBLE
//     with the frame's and the sets `FrameRecorder` already bound stay bound;
//   * reads the same per-view block the opaque pass reads, at the same set and binding, through
//     `import cy.frame` — so a particle and a mesh cannot disagree about the projection;
//   * attaches through `PassExtension`, which is the seam `FrameRecorder` published, and draws
//     inside the frame's own transparent stage.
//
// If any of those three seams were wrong, this module would not draw. That is the test.
//
// ================================================================================================
// WHAT IT IS AND WHAT IT IS NOT
// ================================================================================================
//
// **It is the compositing half of `vfx-system` and nothing else.** It takes a span of records and
// issues ONE draw: no simulation, no emitter, no graph, no attribute layout. `src/vfx/` owns all of
// those, and the seam between the two is `ParticleInstance` — a position, a size and a colour,
// which is the smallest thing a simulation can publish and a renderer can draw.
//
// **It has no atlas and no soft depth fade.** Both are texture work and both are `vfx-system`'s
// data interfaces rather than this layer's; the sprite is a smooth radial falloff computed in the
// fragment shader. The absence is recorded rather than hidden, because a particle renderer that
// quietly drew squares would look like a defect in the simulation.
//
// **The blend is premultiplied**, so one pipeline composites an additive spark (alpha towards 0)
// and an opaque puff (alpha towards 1) without a second blend state.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/pipeline/frame_recorder.h>

namespace cy::rendering::particles {

using pipeline::ExtensionContext;
using pipeline::FrameBindings;
using pipeline::FramePipelines;
using pipeline::PassExtension;

/// One particle, as a simulation publishes it and this module draws it. 32 bytes.
///
/// CAMERA-RELATIVE (design.md §3). There is no world-space position in this struct and nowhere to
/// put one, which is the same decision `GpuLight` and `cy/view.slang` each record for themselves.
struct alignas(16) ParticleInstance {
    /// Metres, relative to the view's camera.
    f32 position[3] = {0.0F, 0.0F, 0.0F};
    /// Half-extent in metres. A zero-size particle is drawn and covers nothing, which is what a
    /// simulation's dead slot should cost rather than a branch on the CPU.
    f32 size = 0.0F;
    /// Linear, un-premultiplied. `a` is the opacity the shader premultiplies by its own falloff.
    f32 color[4] = {1.0F, 1.0F, 1.0F, 1.0F};
};

static_assert(sizeof(ParticleInstance) == 32);

/// What one recorded effect did.
struct ParticleReport {
    u32 particles = 0;
    /// Particles the ring could not hold. Counted rather than refused: a frame that dropped the
    /// tail of an effect is a frame that still renders, and `vfx-system`'s importance classes are
    /// what decide which tail. A number here is what makes that decision measurable.
    u32 dropped = 0;
    u32 draws = 0;
};

/// The sprite pipeline, its ring, and the extension that puts it in the frame.
class ParticleRenderer {
public:
    ParticleRenderer() noexcept = default;
    ~ParticleRenderer();

    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;
    ParticleRenderer(ParticleRenderer&&) = delete;
    ParticleRenderer& operator=(ParticleRenderer&&) = delete;

    /// Create the pipeline and the ring. `pipelines` must be initialized: its set layouts 0 and 1
    /// are reused verbatim, which is what makes this pipeline's layout compatible with the frame's.
    [[nodiscard]] Status initialize(rhi::Device& device, const FramePipelines& pipelines,
                                    u32 capacity) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }

    /// Write one frame's particles into the ring slot `begin_frame()` returned.
    ///
    /// CALL IT INSIDE THE HOST'S DEVICE FRAME and before `FrameAssembly::execute`, for the reason
    /// `FrameBindings::upload` gives: the set is allocated from that frame's pool.
    [[nodiscard]] Status upload(u32 frame_slot, Span<const ParticleInstance> particles) noexcept;

    /// The extension to hand `FrameRecorder::add_extension`. Attached to the TRANSPARENT stage:
    /// after the transparent draws, before the tonemap, which is where a composited effect belongs.
    [[nodiscard]] PassExtension extension() noexcept;

    [[nodiscard]] const ParticleReport& report() const noexcept { return report_; }
    void reset_report() noexcept { report_ = ParticleReport{}; }

    // --- What the extension's callback reads. Public because `ExtensionRecordFn` is a plain
    // function pointer and the callback is a free function.

    [[nodiscard]] const FramePipelines* pipelines() const noexcept { return pipelines_; }
    [[nodiscard]] rhi::PipelineLayoutHandle layout() const noexcept { return layout_; }
    [[nodiscard]] rhi::GraphicsPipelineHandle pipeline() const noexcept { return pipeline_; }
    [[nodiscard]] rhi::DescriptorSetHandle set() const noexcept { return set_; }
    [[nodiscard]] u32 live() const noexcept { return live_; }
    [[nodiscard]] ParticleReport& mutable_report() noexcept { return report_; }

private:
    [[nodiscard]] Status create_pipeline(rhi::Device& device,
                                         const FramePipelines& pipelines) noexcept;

    rhi::Device* device_ = nullptr;
    const FramePipelines* pipelines_ = nullptr;
    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragment_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle layout_;
    rhi::GraphicsPipelineHandle pipeline_;
    rhi::BufferHandle rings_[rhi::kMaxFramesInFlight];
    rhi::DescriptorSetHandle set_;
    u32 slot_count_ = 0;
    u32 capacity_ = 0;
    u32 live_ = 0;
    ParticleReport report_;
    bool ready_ = false;
};

}  // namespace cy::rendering::particles
