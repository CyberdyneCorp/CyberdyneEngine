#pragma once
// ONE PIPELINE, ONE RING, ONE DRAW — what the sprite renderer and the strip renderer share.
// An implementation detail of `cy::rendering-particles`, in a public header only because the two
// renderers hold one by value.
//
// Both renderers are the same arrangement with a different shader and a different record: a
// pipeline whose layout REUSES the frame's set layouts 0 and 1 and push range (so the sets the
// frame bound stay bound — `particle_renderer.h` says why each half of that is load-bearing), a
// storage buffer per frame in flight at set 2, and a premultiplied blend that is depth-tested and
// not depth-written. Written once here, because the two rules that took validation errors to find
// — identical set layout handles and an identical push-constant range — are rules a second copy
// would have to get right again.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/pipeline/frame_pipelines.h>

namespace cy::rendering::particles::detail {

/// One stage of a transparent draw's shader, in both native forms.
struct StageSource {
    const char* name = nullptr;
    /// SPIR-V words, for Vulkan.
    Span<const u32> spirv;
    /// MSL source text and its entry point, for Metal.
    Span<const u8> msl;
    const char* msl_entry = nullptr;
};

/// What one transparent draw is made of.
struct TransparentDrawDescription {
    /// The pipeline's and the ring's debug names.
    const char* name = nullptr;
    StageSource vertex;
    StageSource fragment;
    /// Bytes one record occupies in the ring.
    u32 record_size = 0;
    /// Records the ring holds.
    u32 capacity = 0;
};

class TransparentDraw {
public:
    TransparentDraw() noexcept = default;
    ~TransparentDraw();

    TransparentDraw(const TransparentDraw&) = delete;
    TransparentDraw& operator=(const TransparentDraw&) = delete;
    TransparentDraw(TransparentDraw&&) = delete;
    TransparentDraw& operator=(TransparentDraw&&) = delete;

    [[nodiscard]] Status initialize(rhi::Device& device, const pipeline::FramePipelines& pipelines,
                                    const TransparentDrawDescription& description) noexcept;
    void shutdown() noexcept;

    /// Copy `count` records into the ring slot `frame_slot` and allocate this frame's set for it.
    /// `count` must already be clamped to the capacity; the caller is the one that reports a drop.
    [[nodiscard]] Status upload(u32 frame_slot, const void* records, u32 count) noexcept;

    /// Bind the pipeline and this frame's set. The frame's own sets 0 and 1 are the caller's.
    void bind(rhi::CommandBuffer& commands) const noexcept;

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }
    [[nodiscard]] rhi::PipelineLayoutHandle layout() const noexcept { return layout_; }
    [[nodiscard]] rhi::GraphicsPipelineHandle pipeline() const noexcept { return pipeline_; }
    [[nodiscard]] rhi::DescriptorSetHandle set() const noexcept { return set_; }

private:
    [[nodiscard]] Status create_pipeline(const pipeline::FramePipelines& pipelines,
                                         const TransparentDrawDescription& description) noexcept;

    rhi::Device* device_ = nullptr;
    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragment_;
    rhi::DescriptorSetLayoutHandle set_layout_;
    rhi::PipelineLayoutHandle layout_;
    rhi::GraphicsPipelineHandle pipeline_;
    rhi::BufferHandle rings_[rhi::kMaxFramesInFlight];
    rhi::DescriptorSetHandle set_;
    u32 slot_count_ = 0;
    u32 capacity_ = 0;
    u32 record_size_ = 0;
    bool ready_ = false;
};

}  // namespace cy::rendering::particles::detail
