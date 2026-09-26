#pragma once
// BLOOM, ON THE DEVICE: the recorder for the passes `forward/bloom_chain.h` declares.
//
// `rendering-post-processing`'s Bloom requirement — a progressive downsample and upsample chain, a
// soft threshold knee, a Karis average on the first downsample, a tent filter on the upsample, an
// artistic intensity, energy redistributed rather than added — and step 9 of its chain, on linear
// HDR before exposure.
//
// ================================================================================================
// HOW IT REACHES A FRAME
// ================================================================================================
//
//   AssemblyDescription::post.bloom    the setting. `build_post_chain` puts `PostStage::Bloom` in
//                                      the chain and the assembly turns it into
//                                      `FrameFeatures::bloom`, so ForwardFrame declares the chain.
//                                      Off, the passes and their targets do not exist.
//   FramePassKind::Bloom               every pass of the chain carries this renderer's `sink()`.
//                                      `FrameRecorder::set_bloom` attaches it; a caller that
//                                      records its own passes (samples/12-beauty) attaches `sink()`
//                                      itself.
//   FrameResources::post_source        the post-process reads the bloomed colour from here.
//
// ================================================================================================
// WHAT IT OWNS
// ================================================================================================
//
// Four graphics pipelines — prefilter, downsample, upsample, composite — over the frame's own
// full-screen vertex stage, one pass-set layout (set 2: source, detail, sampler, lens dirt), a
// pipeline layout that names the frame's sets 0 and 1 beside it so its shape is the frame's, and a
// clamped linear sampler. It owns no texture: every level is a graph transient. It allocates one per-frame
// descriptor set per recorded step, because a set a command buffer has bound may not be updated.
//
// D3D12 IS NOT A TARGET YET, and the reason is the vertex stage rather than bloom:
// `fullscreenVertex` takes `SV_VulkanVertexID`, which DXC refuses (declared gap
// `m11c:every-shader-reaches-every-target`, closing at M11.d). `cy/bloom.slang`'s four fragment
// entry points compile to HLSL; a D3D12 port needs that vertex fix, a DXIL copy of these modules,
// and set 2 mapped to its register space.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/forward/bloom_chain.h>
#include <cy/rendering/forward/frame.h>
#include <cy/rendering/post/effects.h>

namespace cy::rendering::pipeline {

class FramePipelines;

/// `cy/bloom.slang`'s `BloomPush`, four float4s.
struct alignas(16) BloomPush {
    /// xy: 1 / source extent. zw: 1 / target extent.
    f32 extents[4] = {};
    /// xy: 1 / detail extent.
    f32 detail[4] = {};
    /// threshold, knee half width (both scene units), 1 / threshold, Karis on.
    f32 threshold[4] = {};
    /// intensity, scatter, anamorphic, lens dirt intensity.
    f32 blend[4] = {};
};

static_assert(sizeof(BloomPush) == 64, "cy/bloom.slang's BloomPush is four float4s");

/// The push block one step records with. A free function so the arithmetic that turns a setting
/// into a shader number is checkable without a device.
[[nodiscard]] BloomPush bloom_push(const BloomStep& step, const BloomSettings& settings) noexcept;

/// What the recording did, read off the recording rather than predicted from the chain.
struct BloomReport {
    u32 steps = 0;
    /// Steps whose pass was not one of the bound chain's, or whose set could not be allocated.
    u32 skipped = 0;
};

class BloomRenderer {
public:
    BloomRenderer() noexcept = default;
    ~BloomRenderer();

    BloomRenderer(const BloomRenderer&) = delete;
    BloomRenderer& operator=(const BloomRenderer&) = delete;
    BloomRenderer(BloomRenderer&&) = delete;
    BloomRenderer& operator=(BloomRenderer&&) = delete;

    /// Create the pipelines against the frame's own: its colour format and its sets 0 and 1.
    [[nodiscard]] Status initialize(rhi::Device& device, const FramePipelines& frame) noexcept;
    /// Release everything. Idempotent; the caller waits for the device first.
    void shutdown() noexcept;
    [[nodiscard]] bool ready() const noexcept { return ready_; }

    void set_settings(const BloomSettings& settings) noexcept { settings_ = settings; }
    [[nodiscard]] const BloomSettings& settings() const noexcept { return settings_; }

    /// The lens dirt mask, stretched over the frame and weighted by
    /// `BloomSettings::lens_dirt_intensity`. The view is the CALLER'S and must already be in the
    /// sampled layout, the way a resident material texture is: the chain's passes do not declare it
    /// to the graph because nothing in the frame writes it. Null, the default, is a clean lens.
    void set_lens_dirt(rhi::TextureViewHandle view) noexcept { lens_dirt_ = view; }

    /// The callback for `FramePassKind::Bloom`, recording the steps of `chain`. The chain is read
    /// when the passes record, so it may be — and in a frame is — filled after this is called.
    [[nodiscard]] FramePassCallback sink(const BloomChain& chain) noexcept;

    [[nodiscard]] const BloomReport& report() const noexcept { return report_; }
    void reset_report() noexcept { report_ = BloomReport{}; }

    /// Record one step. Public because `RecordFn` is a plain function pointer.
    void record(const PassContext& context) noexcept;

private:
    [[nodiscard]] Status create_modules(rhi::Device& device) noexcept;
    [[nodiscard]] Status create_layout(rhi::Device& device, const FramePipelines& frame) noexcept;
    [[nodiscard]] Status create_pipelines(rhi::Device& device, rhi::Format format) noexcept;

    rhi::Device* device_ = nullptr;
    BloomSettings settings_;
    const BloomChain* chain_ = nullptr;
    rhi::TextureViewHandle lens_dirt_;
    rhi::ShaderModuleHandle vertex_;
    rhi::ShaderModuleHandle fragments_[static_cast<u32>(BloomStepKind::Count)];
    rhi::GraphicsPipelineHandle pipelines_[static_cast<u32>(BloomStepKind::Count)];
    rhi::DescriptorSetLayoutHandle pass_set_;
    rhi::PipelineLayoutHandle layout_;
    rhi::SamplerHandle sampler_;
    BloomReport report_;
    bool ready_ = false;
};

}  // namespace cy::rendering::pipeline
