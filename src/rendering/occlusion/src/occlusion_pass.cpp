// SPDX-License-Identifier: MIT
#include <cy/rendering/occlusion/occlusion_pass.h>

#include <cy/backends/rhi/validation.h>
#include <cy/rendering/graph/executor.h>

#include <cstring>

#include "occlusion_msl.h"
#include "occlusion_spirv.h"

namespace cy::rendering::occlusion {
namespace {

using rhi::Access;
using rhi::QueueKind;

constexpr u32 kGroupSize = 8;
constexpr u32 kSearch = 0;
constexpr u32 kFilter = 1;

/// gtao.slang's set: depth, normals, output. gtao_filter.slang's: depth, normals, raw, source,
/// output. The binding index is the field's position in the `ParameterBlock`, which is also the
/// argument-buffer id Metal assigns.
constexpr u32 kSearchBindings = 3;
constexpr u32 kFilterBindings = 5;

[[nodiscard]] u32 groups(u32 extent) noexcept {
    return (extent + kGroupSize - 1U) / kGroupSize;
}

/// IEEE 754 binary16 to binary32, for the readback of an `Rgba16Sfloat` target.
[[nodiscard]] f32 half_to_float(u16 half) noexcept {
    const u32 sign = static_cast<u32>(half & 0x8000U) << 16U;
    const u32 exponent = (half >> 10U) & 0x1FU;
    u32 mantissa = half & 0x3FFU;
    u32 bits = 0;
    if (exponent == 0U) {
        if (mantissa == 0U) {
            bits = sign;
        } else {
            // Subnormal: renormalise into binary32's range.
            u32 shift = 0;
            while ((mantissa & 0x400U) == 0U) {
                mantissa <<= 1U;
                ++shift;
            }
            mantissa &= 0x3FFU;
            bits = sign | ((113U - shift) << 23U) | (mantissa << 13U);
        }
    } else if (exponent == 0x1FU) {
        bits = sign | 0x7F800000U | (mantissa << 13U);
    } else {
        bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    }
    f32 value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] rhi::DescriptorWrite sampled(u32 binding, rhi::TextureViewHandle view) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind = rhi::DescriptorKind::SampledTexture;
    write.texture_view = view;
    write.use = rhi::ImageUse::SampledRead;
    return write;
}

[[nodiscard]] rhi::DescriptorWrite storage(u32 binding, rhi::TextureViewHandle view) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind = rhi::DescriptorKind::StorageTexture;
    write.texture_view = view;
    write.use = rhi::ImageUse::Storage;
    return write;
}

}  // namespace

AmbientOcclusionPass::~AmbientOcclusionPass() {
    destroy();
}

bool AmbientOcclusionPass::supported(const rhi::Device& device) noexcept {
    const rhi::ShaderFormat format = device.capabilities().native_shader_format();
    return device.capabilities().has(rhi::Capability::ComputeShaders) &&
           (format == rhi::ShaderFormat::Spirv || format == rhi::ShaderFormat::Msl);
}

Status AmbientOcclusionPass::create(Allocator& allocator, rhi::Device& device,
                                    const AmbientOcclusionPassDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the ambient occlusion pass is already created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "ambient occlusion is a chain of compute dispatches and this device has no "
                    "compute stage or no shader format this module ships (SPIR-V or MSL)");
    }
    if (desc.width == 0 || desc.height == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "AmbientOcclusionPassDescription::width and ::height must be non-zero");
    }
    if (filter_pass_count() == 0 || filter_pass_count() > kMaxFilterPasses) {
        return fail(ErrorCode::OutOfRange,
                    "the denoiser's ambient occlusion cascade is outside what this pass records");
    }
    allocator_ = &allocator;
    device_ = &device;
    desc_ = desc;
    if (Status created = create_pipelines(); !created) {
        destroy();
        return created;
    }
    if (Status created = create_resources(); !created) {
        destroy();
        return created;
    }
    return ok();
}

Status AmbientOcclusionPass::create_pipelines() noexcept {
    if (Status created = create_pipeline("gtao horizons", false); !created) {
        return created;
    }
    return create_pipeline("gtao filter", true);
}

Status AmbientOcclusionPass::create_pipeline(const char* name, bool filter) noexcept {
    const u32 which = filter ? kFilter : kSearch;
    rhi::ShaderModuleBundle bundle;
    if (filter) {
        bundle.spirv = {kGtaoFilterSpirv, sizeof(kGtaoFilterSpirv) / sizeof(u32)};
        bundle.msl = {reinterpret_cast<const u8*>(kGtaoFilterMsl), sizeof(kGtaoFilterMsl) - 1};
        bundle.msl_entry_point = "cyGtaoFilter";
    } else {
        bundle.spirv = {kGtaoSpirv, sizeof(kGtaoSpirv) / sizeof(u32)};
        bundle.msl = {reinterpret_cast<const u8*>(kGtaoMsl), sizeof(kGtaoMsl) - 1};
        bundle.msl_entry_point = "cyGtao";
    }
    bundle.spirv_entry_point = "main";
    rhi::ValidationMessage message;
    auto module = rhi::select_shader_module(bundle, device_->capabilities().native_shader_format(),
                                            name, rhi::ShaderStage::Compute, message);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    Expected<rhi::ShaderModuleHandle, Error> shader = device_->create_shader_module(*module);
    if (!shader.has_value()) {
        return make_unexpected(shader.error());
    }
    shaders_[which] = *shader;

    const u32 count = filter ? kFilterBindings : kSearchBindings;
    rhi::DescriptorBinding bindings[kFilterBindings] = {};
    for (u32 index = 0; index < count; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = index + 1U == count ? rhi::DescriptorKind::StorageTexture
                                                   : rhi::DescriptorKind::SampledTexture;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = name;
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, count);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layouts_[which] = *layout;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0,
                                       filter ? static_cast<u32>(sizeof(GtaoFilterConstants))
                                              : static_cast<u32>(sizeof(GtaoConstants))};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = name;
    pipeline_layout.set_layouts =
        Span<const rhi::DescriptorSetLayoutHandle>(&set_layouts_[which], 1);
    pipeline_layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> made =
        device_->create_pipeline_layout(pipeline_layout);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    pipeline_layouts_[which] = *made;

    rhi::ComputePipelineDescription pipeline;
    pipeline.name = name;
    pipeline.layout = pipeline_layouts_[which];
    pipeline.shader = shaders_[which];
    pipeline.workgroup_size[0] = kGroupSize;
    pipeline.workgroup_size[1] = kGroupSize;
    Expected<rhi::ComputePipelineHandle, Error> created =
        device_->create_compute_pipeline(pipeline);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    pipelines_[which] = *created;
    return ok();
}

Status AmbientOcclusionPass::create_resources() noexcept {
    rhi::TextureDescription target;
    target.name = "ambient occlusion";
    target.format = target_format();
    target.extent = rhi::Extent3D{desc_.width, desc_.height, 1};
    target.usage =
        rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage | rhi::TextureUsage::TransferSource;
    Expected<rhi::TextureHandle, Error> texture = device_->create_texture(target);
    if (!texture.has_value()) {
        return make_unexpected(texture.error());
    }
    target_ = *texture;
    rhi::TextureViewDescription view;
    view.name = "ambient occlusion";
    view.texture = target_;
    Expected<rhi::TextureViewHandle, Error> made = device_->create_texture_view(view);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    target_view_ = *made;

    if (!desc_.readback) {
        return ok();
    }
    rhi::BufferDescription host;
    host.size = static_cast<u64>(desc_.width) * desc_.height * 8U;
    host.usage = rhi::BufferUsage::TransferDestination;
    host.memory = rhi::MemoryUse::Readback;
    host.name = "ambient occlusion readback";
    Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(host);
    if (!buffer.has_value()) {
        return make_unexpected(buffer.error());
    }
    readback_ = *buffer;
    host.name = "ambient occlusion raw readback";
    buffer = device_->create_buffer(host);
    if (!buffer.has_value()) {
        return make_unexpected(buffer.error());
    }
    raw_readback_ = *buffer;
    return ok();
}

void AmbientOcclusionPass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    if (!target_view_.is_null()) {
        device_->destroy_texture_view(target_view_);
    }
    if (!target_.is_null()) {
        device_->destroy_texture(target_);
    }
    for (rhi::BufferHandle* buffer : {&readback_, &raw_readback_}) {
        if (!buffer->is_null()) {
            device_->destroy_buffer(*buffer);
        }
        *buffer = {};
    }
    for (u32 which = 0; which < 2; ++which) {
        if (!pipelines_[which].is_null()) {
            device_->destroy_compute_pipeline(pipelines_[which]);
        }
        if (!pipeline_layouts_[which].is_null()) {
            device_->destroy_pipeline_layout(pipeline_layouts_[which]);
        }
        if (!set_layouts_[which].is_null()) {
            device_->destroy_descriptor_set_layout(set_layouts_[which]);
        }
        if (!shaders_[which].is_null()) {
            device_->destroy_shader_module(shaders_[which]);
        }
        pipelines_[which] = {};
        pipeline_layouts_[which] = {};
        set_layouts_[which] = {};
        shaders_[which] = {};
    }
    target_view_ = {};
    target_ = {};
    imported_ = kInvalidResource;
    device_ = nullptr;
    allocator_ = nullptr;
}

Status AmbientOcclusionPass::set_view(const GtaoView& view) noexcept {
    if (view.width != desc_.width || view.height != desc_.height) {
        return fail(ErrorCode::InvalidArgument,
                    "ambient occlusion: the view's extent is not the one the pass was created at");
    }
    Expected<GtaoConstants, Error> constants = make_gtao_constants(settings_, view);
    if (!constants.has_value()) {
        return make_unexpected(constants.error());
    }
    view_ = view;
    constants_ = *constants;
    return ok();
}

ResourceId AmbientOcclusionPass::import_target(RenderGraph& graph) noexcept {
    TextureRequest request;
    request.name = "ambient occlusion";
    request.format = target_format();
    request.width = desc_.width;
    request.height = desc_.height;
    request.extra_usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSource;
    // UNDEFINED EVERY FRAME, and it is the truth: the last filter pass writes every texel, so
    // whatever the previous frame left is discarded rather than transitioned.
    imported_ = graph.import_texture(request, target_, rhi::ImageUse::Undefined);
    return imported_;
}

FrameStageDeclaration AmbientOcclusionPass::stage() noexcept {
    return FrameStageDeclaration{&declare_stage, this};
}

PassId AmbientOcclusionPass::declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                           void* user) noexcept {
    return static_cast<AmbientOcclusionPass*>(user)->declare(graph, inputs);
}

PassId AmbientOcclusionPass::declare(RenderGraph& graph,
                                     const ScreenSpaceStageInputs& inputs) noexcept {
    declared_passes_ = 0;
    // Each refusal is a frame that would otherwise sample a term nothing wrote, or write one the
    // forward pass does not read.
    if (device_ == nullptr || view_.width == 0 || inputs.target == kInvalidResource ||
        inputs.target != imported_ || inputs.depth == kInvalidResource ||
        inputs.normal_roughness == kInvalidResource || inputs.width != desc_.width ||
        inputs.height != desc_.height) {
        return kInvalidPass;
    }

    TextureRequest request;
    request.format = target_format();
    request.width = desc_.width;
    request.height = desc_.height;
    request.name = "ambient occlusion horizons";
    const ResourceId raw = graph.create_texture(request);
    request.name = "ambient occlusion filter a";
    const ResourceId ping = graph.create_texture(request);
    request.name = "ambient occlusion filter b";
    const ResourceId pong = graph.create_texture(request);

    search_ = Step{};
    search_.self = this;
    search_.depth = inputs.depth;
    search_.normals = inputs.normal_roughness;
    search_.output = raw;
    const PassId first = graph.add_pass("ambient occlusion horizons", QueueKind::Graphics)
                             .read(inputs.depth, Access::ComputeSampledRead)
                             .read(inputs.normal_roughness, Access::ComputeSampledRead)
                             .write(raw, Access::ComputeStorageWrite)
                             .record(&record_search, &search_)
                             .id();
    ++declared_passes_;

    const u32 passes = filter_pass_count();
    ResourceId source = raw;
    for (u32 pass = 0; pass < passes; ++pass) {
        // The last pass writes the target; the ones before it alternate between the two scratch
        // images, so no pass reads what it writes.
        const ResourceId scratch = (pass & 1U) == 0U ? ping : pong;
        const ResourceId output = pass + 1U == passes ? inputs.target : scratch;
        Step& step = filters_[pass];
        step = Step{};
        step.self = this;
        step.depth = inputs.depth;
        step.normals = inputs.normal_roughness;
        step.raw = raw;
        step.source = source;
        step.output = output;
        step.filter = make_filter_constants(view_, 1U << pass);
        PassBuilder builder = graph.add_pass("ambient occlusion filter", QueueKind::Graphics);
        builder.read(inputs.depth, Access::ComputeSampledRead)
            .read(inputs.normal_roughness, Access::ComputeSampledRead)
            .read(raw, Access::ComputeSampledRead);
        if (source != raw) {
            builder.read(source, Access::ComputeSampledRead);
        }
        builder.write(output, Access::ComputeStorageWrite).record(&record_filter, &step);
        ++declared_passes_;
        source = output;
    }

    if (desc_.readback) {
        declare_readback(graph, readbacks_[0], inputs.target, "ambient occlusion readback");
        declare_readback(graph, readbacks_[1], raw, "ambient occlusion raw readback");
    }
    return first;
}

void AmbientOcclusionPass::declare_readback(RenderGraph& graph, Readback& readback,
                                            ResourceId source, const char* name) noexcept {
    readback.self = this;
    readback.source = source;
    readback.buffer = source == imported_ ? readback_ : raw_readback_;
    BufferRequest request;
    request.name = name;
    request.size = static_cast<u64>(desc_.width) * desc_.height * 8U;
    request.extra_usage = rhi::BufferUsage::TransferDestination;
    const ResourceId destination = graph.import_buffer(request, readback.buffer);
    graph.add_pass(name, QueueKind::Graphics)
        .read(source, Access::TransferRead)
        .write(destination, Access::TransferWrite)
        .record(&record_readback, &readback);
    // The host boundary as a declared read, so the graph derives the transfer-to-host barrier.
    graph.add_pass("ambient occlusion host", QueueKind::Graphics)
        .read(destination, Access::HostRead)
        .side_effect();
    declared_passes_ += 2;
}

void AmbientOcclusionPass::record_search(const PassContext& context, void* user) noexcept {
    const auto* step = static_cast<const Step*>(user);
    AmbientOcclusionPass& self = *step->self;
    Expected<rhi::DescriptorSetHandle, Error> set =
        self.device_->allocate_descriptor_set(self.set_layouts_[kSearch], true);
    if (!set.has_value()) {
        return;
    }
    const rhi::DescriptorWrite writes[kSearchBindings] = {
        sampled(0, context.executor->view(step->depth)),
        sampled(1, context.executor->view(step->normals)),
        storage(2, context.executor->view(step->output)),
    };
    if (Status written = self.device_->update_descriptor_set(
            *set, Span<const rhi::DescriptorWrite>(writes, kSearchBindings));
        !written) {
        return;
    }
    context.commands->bind_compute_pipeline(self.pipelines_[kSearch]);
    context.commands->bind_descriptor_sets(self.pipeline_layouts_[kSearch], 0,
                                           Span<const rhi::DescriptorSetHandle>(&*set, 1));
    context.commands->push_constants(
        self.pipeline_layouts_[kSearch], rhi::ShaderStage::Compute, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&self.constants_), sizeof(GtaoConstants)));
    context.commands->dispatch(groups(self.desc_.width), groups(self.desc_.height), 1);
}

void AmbientOcclusionPass::record_filter(const PassContext& context, void* user) noexcept {
    const auto* step = static_cast<const Step*>(user);
    AmbientOcclusionPass& self = *step->self;
    Expected<rhi::DescriptorSetHandle, Error> set =
        self.device_->allocate_descriptor_set(self.set_layouts_[kFilter], true);
    if (!set.has_value()) {
        return;
    }
    const rhi::DescriptorWrite writes[kFilterBindings] = {
        sampled(0, context.executor->view(step->depth)),
        sampled(1, context.executor->view(step->normals)),
        sampled(2, context.executor->view(step->raw)),
        sampled(3, context.executor->view(step->source)),
        storage(4, context.executor->view(step->output)),
    };
    if (Status written = self.device_->update_descriptor_set(
            *set, Span<const rhi::DescriptorWrite>(writes, kFilterBindings));
        !written) {
        return;
    }
    context.commands->bind_compute_pipeline(self.pipelines_[kFilter]);
    context.commands->bind_descriptor_sets(self.pipeline_layouts_[kFilter], 0,
                                           Span<const rhi::DescriptorSetHandle>(&*set, 1));
    context.commands->push_constants(
        self.pipeline_layouts_[kFilter], rhi::ShaderStage::Compute, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&step->filter), sizeof(GtaoFilterConstants)));
    context.commands->dispatch(groups(self.desc_.width), groups(self.desc_.height), 1);
}

void AmbientOcclusionPass::record_readback(const PassContext& context, void* user) noexcept {
    const auto* readback = static_cast<const Readback*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent =
        rhi::Extent3D{readback->self->desc_.width, readback->self->desc_.height, 1};
    context.commands->copy_texture_to_buffer(context.executor->texture(readback->source),
                                             readback->buffer,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

Status AmbientOcclusionPass::copy_back(rhi::BufferHandle buffer, Span<Vec4> out) const noexcept {
    if (device_ == nullptr || buffer.is_null()) {
        return fail(ErrorCode::Unavailable,
                    "ambient occlusion: created without `readback`, so there is nothing to read");
    }
    const usize pixels = static_cast<usize>(desc_.width) * desc_.height;
    if (out.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "ambient occlusion: the readback span is not the view");
    }
    const auto* halves = static_cast<const u16*>(device_->buffer_mapped_pointer(buffer));
    if (halves == nullptr) {
        return fail(ErrorCode::Internal, "ambient occlusion: the readback buffer is not mapped");
    }
    for (usize pixel = 0; pixel < pixels; ++pixel) {
        const u16* texel = halves + (pixel * 4U);
        out[pixel] = Vec4{half_to_float(texel[0]), half_to_float(texel[1]), half_to_float(texel[2]),
                          half_to_float(texel[3])};
    }
    return ok();
}

Status AmbientOcclusionPass::read_back(Span<Vec4> out) const noexcept {
    return copy_back(readback_, out);
}

Status AmbientOcclusionPass::read_back_raw(Span<Vec4> out) const noexcept {
    return copy_back(raw_readback_, out);
}

}  // namespace cy::rendering::occlusion
