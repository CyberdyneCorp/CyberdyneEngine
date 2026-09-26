// Bloom's device recorder. See the header for how it reaches a frame.

#include <cy/rendering/pipeline/bloom_renderer.h>

#include "bloom_msl.h"
#include "bloom_spirv.h"
#include "frame_msl.h"
#include "frame_spirv.h"

#include <cy/core/math/scalar.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/pipeline/frame_pipelines.h>

namespace cy::rendering::pipeline {
namespace {

inline constexpr u32 kStepKinds = static_cast<u32>(BloomStepKind::Count);
inline constexpr u32 kBindingSource = 0;
inline constexpr u32 kBindingDetail = 1;
inline constexpr u32 kBindingSampler = 2;
inline constexpr u32 kBindingLensDirt = 3;
inline constexpr u32 kBindingCount = 4;

template <typename T, usize N>
[[nodiscard]] Span<const u32> words(const T (&module)[N]) noexcept {
    return Span<const u32>(module, N);
}

[[nodiscard]] f32 reciprocal(u32 extent) noexcept {
    return extent == 0 ? 0.0F : 1.0F / static_cast<f32>(extent);
}

void record_step(const PassContext& context, void* user) noexcept {
    static_cast<BloomRenderer*>(user)->record(context);
}

[[nodiscard]] rhi::DescriptorBinding pass_binding(u32 binding, rhi::DescriptorKind kind) noexcept {
    rhi::DescriptorBinding entry;
    entry.binding = binding;
    entry.kind = kind;
    entry.count = 1;
    entry.stages = rhi::ShaderStage::Fragment;
    return entry;
}

}  // namespace

BloomPush bloom_push(const BloomStep& step, const BloomSettings& settings) noexcept {
    BloomPush push;
    push.extents[0] = reciprocal(step.source_width);
    push.extents[1] = reciprocal(step.source_height);
    push.extents[2] = reciprocal(step.target_width);
    push.extents[3] = reciprocal(step.target_height);
    push.detail[0] = reciprocal(step.detail_width);
    push.detail[1] = reciprocal(step.detail_height);
    // The same numbers `bloom_prefilter` derives, so the shader's knee is the processor's knee.
    const f32 threshold = math::max(settings.threshold, 0.0F);
    push.threshold[0] = threshold;
    push.threshold[1] = math::max(settings.knee, 0.0F) * threshold;
    push.threshold[2] = 1.0F / math::max(threshold, 1e-6F);
    push.threshold[3] = settings.firefly_suppression ? 1.0F : 0.0F;
    push.blend[0] = math::saturate(settings.intensity);
    push.blend[1] = math::saturate(settings.scatter);
    push.blend[2] = math::max(settings.anamorphic, 0.0F);
    push.blend[3] = math::max(settings.lens_dirt_intensity, 0.0F);
    return push;
}

BloomRenderer::~BloomRenderer() {
    shutdown();
}

Status BloomRenderer::create_modules(rhi::Device& device) noexcept {
    struct Request {
        const char* name;
        rhi::ShaderStage stage;
        const char* msl_entry;
        Span<const u32> spirv;
        const char* msl;
        usize msl_bytes;
        rhi::ShaderModuleHandle* out;
    };
    // The vertex stage is the frame's own full-screen triangle, the module `FramePipelines`
    // creates for its resolve — the same words, not a second copy of the shader.
    const Request requests[] = {
        {"cy bloom vertex", rhi::ShaderStage::Vertex, "fullscreenVertex",
         words(kFrameResolveVertexSpirv), kFrameResolveVertexMsl,
         sizeof(kFrameResolveVertexMsl) - 1, &vertex_},
        {"cy bloom prefilter", rhi::ShaderStage::Fragment, "bloomPrefilter",
         words(kBloomPrefilterSpirv), kBloomPrefilterMsl, sizeof(kBloomPrefilterMsl) - 1,
         &fragments_[static_cast<u32>(BloomStepKind::Prefilter)]},
        {"cy bloom downsample", rhi::ShaderStage::Fragment, "bloomDownsample",
         words(kBloomDownsampleSpirv), kBloomDownsampleMsl, sizeof(kBloomDownsampleMsl) - 1,
         &fragments_[static_cast<u32>(BloomStepKind::Downsample)]},
        {"cy bloom upsample", rhi::ShaderStage::Fragment, "bloomUpsample",
         words(kBloomUpsampleSpirv), kBloomUpsampleMsl, sizeof(kBloomUpsampleMsl) - 1,
         &fragments_[static_cast<u32>(BloomStepKind::Upsample)]},
        {"cy bloom composite", rhi::ShaderStage::Fragment, "bloomComposite",
         words(kBloomCompositeSpirv), kBloomCompositeMsl, sizeof(kBloomCompositeMsl) - 1,
         &fragments_[static_cast<u32>(BloomStepKind::Composite)]},
    };
    const bool metal = device.capabilities().native_shader_format() == rhi::ShaderFormat::Msl;
    for (const Request& request : requests) {
        rhi::ShaderModuleDescription description;
        description.name = request.name;
        description.stage = request.stage;
        if (metal) {
            description.entry_point = request.msl_entry;
            description.native =
                Span<const u8>(reinterpret_cast<const u8*>(request.msl), request.msl_bytes);
            description.native_format = rhi::ShaderFormat::Msl;
        } else {
            description.entry_point = "main";
            description.spirv = request.spirv;
        }
        Expected<rhi::ShaderModuleHandle, Error> module = device.create_shader_module(description);
        if (!module.has_value()) {
            return make_unexpected(module.error());
        }
        *request.out = *module;
    }
    return ok();
}

Status BloomRenderer::create_layout(rhi::Device& device, const FramePipelines& frame) noexcept {
    const rhi::DescriptorBinding bindings[] = {
        pass_binding(kBindingSource, rhi::DescriptorKind::SampledTexture),
        pass_binding(kBindingDetail, rhi::DescriptorKind::SampledTexture),
        pass_binding(kBindingSampler, rhi::DescriptorKind::Sampler),
        pass_binding(kBindingLensDirt, rhi::DescriptorKind::SampledTexture),
    };
    rhi::DescriptorSetLayoutDescription set;
    set.name = "cy bloom pass";
    set.bindings = Span<const rhi::DescriptorBinding>(bindings, kBindingCount);
    Expected<rhi::DescriptorSetLayoutHandle, Error> made = device.create_descriptor_set_layout(set);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    pass_set_ = *made;

    // The frame's sets 0 and 1 in their places, so set 2 is set 2 on every backend — Metal binds a
    // layout's sets at their own indices and its push block after the last of them.
    const rhi::DescriptorSetLayoutHandle sets[kSetCount] = {frame.set_layout(kGlobalSet),
                                                            frame.set_layout(kViewSet), pass_set_};
    const rhi::PushConstantRange range{rhi::ShaderStage::Fragment, 0, sizeof(BloomPush)};
    rhi::PipelineLayoutDescription layout;
    layout.name = "cy bloom layout";
    layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(sets, kSetCount);
    layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> created = device.create_pipeline_layout(layout);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    layout_ = *created;

    rhi::SamplerDescription sampler;
    sampler.name = "cy bloom linear clamp";
    sampler.address_u = rhi::AddressMode::ClampToEdge;
    sampler.address_v = rhi::AddressMode::ClampToEdge;
    sampler.address_w = rhi::AddressMode::ClampToEdge;
    sampler.mipmap_mode = rhi::MipmapMode::Nearest;
    Expected<rhi::SamplerHandle, Error> sampled = device.create_sampler(sampler);
    if (!sampled.has_value()) {
        return make_unexpected(sampled.error());
    }
    sampler_ = *sampled;
    return ok();
}

Status BloomRenderer::create_pipelines(rhi::Device& device, rhi::Format format) noexcept {
    rhi::ColorAttachmentState color;
    color.format = format;
    for (u32 kind = 0; kind < kStepKinds; ++kind) {
        rhi::GraphicsPipelineDescription description;
        description.name = bloom_step_name(static_cast<BloomStepKind>(kind));
        description.layout = layout_;
        description.vertex_shader = vertex_;
        description.fragment_shader = fragments_[kind];
        description.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
        description.rasterisation.cull_mode = rhi::CullMode::None;
        description.depth_stencil.format = rhi::Format::Undefined;
        description.depth_stencil.depth_test_enable = false;
        description.depth_stencil.depth_write_enable = false;
        Expected<rhi::GraphicsPipelineHandle, Error> created =
            device.create_graphics_pipeline(description);
        if (!created.has_value()) {
            return make_unexpected(created.error());
        }
        pipelines_[kind] = *created;
    }
    return ok();
}

Status BloomRenderer::initialize(rhi::Device& device, const FramePipelines& frame) noexcept {
    if (ready_) {
        return fail(ErrorCode::InvalidArgument, "bloom: already initialized");
    }
    if (!frame.ready()) {
        return fail(ErrorCode::InvalidArgument,
                    "bloom: the frame's pipelines are not ready, and bloom's layout names their "
                    "sets 0 and 1");
    }
    device_ = &device;
    Status made = create_modules(device);
    if (made) {
        made = create_layout(device, frame);
    }
    if (made) {
        made = create_pipelines(device, frame.setup().color_format);
    }
    if (!made) {
        shutdown();
        return made;
    }
    ready_ = true;
    return ok();
}

void BloomRenderer::shutdown() noexcept {
    if (device_ == nullptr) {
        return;
    }
    rhi::Device& device = *device_;
    for (rhi::GraphicsPipelineHandle& pipeline : pipelines_) {
        if (!pipeline.is_null()) {
            device.destroy_graphics_pipeline(pipeline);
            pipeline = rhi::GraphicsPipelineHandle{};
        }
    }
    if (!layout_.is_null()) {
        device.destroy_pipeline_layout(layout_);
        layout_ = rhi::PipelineLayoutHandle{};
    }
    if (!pass_set_.is_null()) {
        device.destroy_descriptor_set_layout(pass_set_);
        pass_set_ = rhi::DescriptorSetLayoutHandle{};
    }
    if (!sampler_.is_null()) {
        device.destroy_sampler(sampler_);
        sampler_ = rhi::SamplerHandle{};
    }
    for (rhi::ShaderModuleHandle& module : fragments_) {
        if (!module.is_null()) {
            device.destroy_shader_module(module);
            module = rhi::ShaderModuleHandle{};
        }
    }
    if (!vertex_.is_null()) {
        device.destroy_shader_module(vertex_);
        vertex_ = rhi::ShaderModuleHandle{};
    }
    device_ = nullptr;
    chain_ = nullptr;
    lens_dirt_ = rhi::TextureViewHandle{};
    ready_ = false;
}

FramePassCallback BloomRenderer::sink(const BloomChain& chain) noexcept {
    chain_ = &chain;
    return FramePassCallback{&record_step, this};
}

void BloomRenderer::record(const PassContext& context) noexcept {
    const BloomStep* step = chain_ != nullptr ? chain_->step_of(context.pass) : nullptr;
    if (!ready_ || step == nullptr) {
        ++report_.skipped;
        return;
    }
    const GraphExecutor& executor = *context.executor;

    // A FRESH SET PER STEP, for `FrameBindings::reallocate_pass_set`'s reason: the source and
    // target views exist only once the graph has realised the transients, and a set an earlier
    // step of the same command buffer bound may not be written again.
    Expected<rhi::DescriptorSetHandle, Error> set =
        device_->allocate_descriptor_set(pass_set_, true);
    if (!set.has_value()) {
        ++report_.skipped;
        return;
    }
    const rhi::TextureViewHandle source = executor.view(step->source);
    // The prefilter and the downsamples read no detail; the binding is written with the source so
    // that no step binds a set with an unwritten descriptor in it.
    const rhi::TextureViewHandle detail =
        step->detail != kInvalidResource ? executor.view(step->detail) : source;
    rhi::DescriptorWrite writes[kBindingCount];
    writes[0].binding = kBindingSource;
    writes[0].kind = rhi::DescriptorKind::SampledTexture;
    writes[0].texture_view = source;
    writes[1].binding = kBindingDetail;
    writes[1].kind = rhi::DescriptorKind::SampledTexture;
    writes[1].texture_view = detail;
    writes[2].binding = kBindingSampler;
    writes[2].kind = rhi::DescriptorKind::Sampler;
    writes[2].sampler = sampler_;
    writes[3].binding = kBindingLensDirt;
    writes[3].kind = rhi::DescriptorKind::SampledTexture;
    writes[3].texture_view = lens_dirt_.is_null() ? source : lens_dirt_;
    if (Status written = device_->update_descriptor_set(*set, {writes, kBindingCount}); !written) {
        ++report_.skipped;
        return;
    }

    rhi::RenderAttachment target;
    target.view = executor.view(step->target);
    // Every texel of every target is written by the full-screen triangle, so nothing is loaded.
    target.load = rhi::LoadOp::DontCare;
    target.store = rhi::StoreOp::Store;
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, step->target_width, step->target_height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&target, 1);

    rhi::CommandBuffer& commands = *context.commands;
    commands.begin_rendering(info);
    commands.set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(step->target_width),
                                        static_cast<f32>(step->target_height), 0.0F, 1.0F});
    commands.set_scissor(rhi::Rect2D{0, 0, step->target_width, step->target_height});
    commands.bind_graphics_pipeline(pipelines_[static_cast<u32>(step->kind)]);
    const rhi::DescriptorSetHandle sets[] = {*set};
    commands.bind_descriptor_sets(layout_, kPassSet, {sets, 1});
    BloomPush push = bloom_push(*step, settings_);
    if (lens_dirt_.is_null()) {
        // No mask bound: the descriptor holds the source, and the shader must not read it as dirt.
        push.blend[3] = 0.0F;
    }
    commands.push_constants(layout_, rhi::ShaderStage::Fragment, 0,
                            Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(push)));
    commands.draw(3, 1, 0, 0);
    commands.end_rendering();
    ++report_.steps;
}

}  // namespace cy::rendering::pipeline
