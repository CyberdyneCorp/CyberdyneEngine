// SPDX-License-Identifier: MIT
#include <cy/rendering/contact_shadows/contact_pass.h>

#include <cy/backends/rhi/validation.h>
#include <cy/rendering/graph/executor.h>

#include "contact_msl.h"
#include "contact_spirv.h"

namespace cy::rendering::contact_shadows {
namespace {

using rhi::Access;
using rhi::QueueKind;

constexpr u32 kGroupSize = 8;
/// contact_shadows.slang's set: depth, normals, output — the `ParameterBlock`'s field order, which
/// is also the argument-buffer id Metal assigns.
constexpr u32 kBindings = 3;

[[nodiscard]] u32 groups(u32 extent) noexcept {
    return (extent + kGroupSize - 1U) / kGroupSize;
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

[[nodiscard]] u64 target_bytes(const ContactShadowPassDescription& desc) noexcept {
    return static_cast<u64>(desc.width) * desc.height * 4U;
}

}  // namespace

ContactShadowPass::~ContactShadowPass() {
    destroy();
}

bool ContactShadowPass::supported(const rhi::Device& device) noexcept {
    const rhi::ShaderFormat format = device.capabilities().native_shader_format();
    return device.capabilities().has(rhi::Capability::ComputeShaders) &&
           (format == rhi::ShaderFormat::Spirv || format == rhi::ShaderFormat::Msl);
}

Status ContactShadowPass::create(Allocator& allocator, rhi::Device& device,
                                 const ContactShadowPassDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the contact shadow pass is already created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "contact shadows are a compute dispatch and this device has no compute stage "
                    "or no shader format this module ships (SPIR-V or MSL)");
    }
    if (desc.width == 0 || desc.height == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "ContactShadowPassDescription::width and ::height must be non-zero");
    }
    allocator_ = &allocator;
    device_ = &device;
    desc_ = desc;
    if (Status created = create_pipeline(); !created) {
        destroy();
        return created;
    }
    if (Status created = create_resources(); !created) {
        destroy();
        return created;
    }
    return ok();
}

Status ContactShadowPass::create_pipeline() noexcept {
    const char* name = "contact shadows";
    rhi::ShaderModuleBundle bundle;
    bundle.spirv = {kContactShadowsSpirv, sizeof(kContactShadowsSpirv) / sizeof(u32)};
    bundle.msl = {reinterpret_cast<const u8*>(kContactShadowsMsl), sizeof(kContactShadowsMsl) - 1};
    bundle.msl_entry_point = "cyContactShadows";
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
    shader_ = *shader;

    rhi::DescriptorBinding bindings[kBindings] = {};
    for (u32 index = 0; index < kBindings; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = index + 1U == kBindings ? rhi::DescriptorKind::StorageTexture
                                                       : rhi::DescriptorKind::SampledTexture;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = name;
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, kBindings);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layout_ = *layout;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0,
                                       static_cast<u32>(sizeof(ContactShadowConstants))};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = name;
    pipeline_layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    pipeline_layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> made =
        device_->create_pipeline_layout(pipeline_layout);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    pipeline_layout_ = *made;

    rhi::ComputePipelineDescription pipeline;
    pipeline.name = name;
    pipeline.layout = pipeline_layout_;
    pipeline.shader = shader_;
    pipeline.workgroup_size[0] = kGroupSize;
    pipeline.workgroup_size[1] = kGroupSize;
    Expected<rhi::ComputePipelineHandle, Error> created =
        device_->create_compute_pipeline(pipeline);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    pipeline_ = *created;
    return ok();
}

Status ContactShadowPass::create_resources() noexcept {
    rhi::TextureDescription target;
    target.name = "contact shadows";
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
    view.name = "contact shadows";
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
    host.size = target_bytes(desc_);
    host.usage = rhi::BufferUsage::TransferDestination;
    host.memory = rhi::MemoryUse::Readback;
    host.name = "contact shadows readback";
    Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(host);
    if (!buffer.has_value()) {
        return make_unexpected(buffer.error());
    }
    readback_ = *buffer;
    return ok();
}

void ContactShadowPass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    if (!target_view_.is_null()) {
        device_->destroy_texture_view(target_view_);
    }
    if (!target_.is_null()) {
        device_->destroy_texture(target_);
    }
    if (!readback_.is_null()) {
        device_->destroy_buffer(readback_);
    }
    if (!pipeline_.is_null()) {
        device_->destroy_compute_pipeline(pipeline_);
    }
    if (!pipeline_layout_.is_null()) {
        device_->destroy_pipeline_layout(pipeline_layout_);
    }
    if (!set_layout_.is_null()) {
        device_->destroy_descriptor_set_layout(set_layout_);
    }
    if (!shader_.is_null()) {
        device_->destroy_shader_module(shader_);
    }
    readback_ = {};
    pipeline_ = {};
    pipeline_layout_ = {};
    set_layout_ = {};
    shader_ = {};
    target_view_ = {};
    target_ = {};
    imported_ = kInvalidResource;
    device_ = nullptr;
    allocator_ = nullptr;
}

Status ContactShadowPass::set_view(const ContactShadowView& view) noexcept {
    if (view.width != desc_.width || view.height != desc_.height) {
        return fail(ErrorCode::InvalidArgument,
                    "contact shadows: the view's extent is not the one the pass was created at");
    }
    Expected<ContactShadowConstants, Error> constants = make_contact_constants(settings_, view);
    if (!constants.has_value()) {
        return make_unexpected(constants.error());
    }
    view_ = view;
    constants_ = *constants;
    return ok();
}

ResourceId ContactShadowPass::import_target(RenderGraph& graph) noexcept {
    TextureRequest request;
    request.name = "contact shadows";
    request.format = target_format();
    request.width = desc_.width;
    request.height = desc_.height;
    request.extra_usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSource;
    // UNDEFINED EVERY FRAME, and it is the truth: the trace writes every texel.
    imported_ = graph.import_texture(request, target_, rhi::ImageUse::Undefined);
    return imported_;
}

FrameStageDeclaration ContactShadowPass::stage() noexcept {
    return FrameStageDeclaration{&declare_stage, this};
}

PassId ContactShadowPass::declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                        void* user) noexcept {
    return static_cast<ContactShadowPass*>(user)->declare(graph, inputs);
}

PassId ContactShadowPass::declare(RenderGraph& graph,
                                  const ScreenSpaceStageInputs& inputs) noexcept {
    // Each refusal is a frame that would otherwise sample a term nothing wrote.
    if (device_ == nullptr || view_.width == 0 || inputs.target == kInvalidResource ||
        inputs.target != imported_ || inputs.depth == kInvalidResource ||
        inputs.normal_roughness == kInvalidResource || inputs.width != desc_.width ||
        inputs.height != desc_.height) {
        return kInvalidPass;
    }
    trace_ = Trace{this, inputs.depth, inputs.normal_roughness, inputs.target};
    const PassId first = graph.add_pass("contact shadows", QueueKind::Graphics)
                             .read(inputs.depth, Access::ComputeSampledRead)
                             .read(inputs.normal_roughness, Access::ComputeSampledRead)
                             .write(inputs.target, Access::ComputeStorageWrite)
                             .record(&record_trace, &trace_)
                             .id();
    if (desc_.readback) {
        copy_ = Readback{this, inputs.target};
        BufferRequest request;
        request.name = "contact shadows readback";
        request.size = target_bytes(desc_);
        request.extra_usage = rhi::BufferUsage::TransferDestination;
        const ResourceId destination = graph.import_buffer(request, readback_);
        graph.add_pass("contact shadows readback", QueueKind::Graphics)
            .read(inputs.target, Access::TransferRead)
            .write(destination, Access::TransferWrite)
            .record(&record_readback, &copy_);
        // The host boundary as a declared read, so the graph derives the transfer-to-host barrier.
        graph.add_pass("contact shadows host", QueueKind::Graphics)
            .read(destination, Access::HostRead)
            .side_effect();
    }
    return first;
}

void ContactShadowPass::record_trace(const PassContext& context, void* user) noexcept {
    const auto* trace = static_cast<const Trace*>(user);
    ContactShadowPass& self = *trace->self;
    Expected<rhi::DescriptorSetHandle, Error> set =
        self.device_->allocate_descriptor_set(self.set_layout_, true);
    if (!set.has_value()) {
        return;
    }
    const rhi::DescriptorWrite writes[kBindings] = {
        sampled(0, context.executor->view(trace->depth)),
        sampled(1, context.executor->view(trace->normals)),
        storage(2, context.executor->view(trace->output)),
    };
    if (Status written = self.device_->update_descriptor_set(
            *set, Span<const rhi::DescriptorWrite>(writes, kBindings));
        !written) {
        return;
    }
    context.commands->bind_compute_pipeline(self.pipeline_);
    context.commands->bind_descriptor_sets(self.pipeline_layout_, 0,
                                           Span<const rhi::DescriptorSetHandle>(&*set, 1));
    context.commands->push_constants(self.pipeline_layout_, rhi::ShaderStage::Compute, 0,
                                     Span<const u8>(reinterpret_cast<const u8*>(&self.constants_),
                                                    sizeof(ContactShadowConstants)));
    context.commands->dispatch(groups(self.desc_.width), groups(self.desc_.height), 1);
}

void ContactShadowPass::record_readback(const PassContext& context, void* user) noexcept {
    const auto* copy = static_cast<const Readback*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{copy->self->desc_.width, copy->self->desc_.height, 1};
    context.commands->copy_texture_to_buffer(context.executor->texture(copy->source),
                                             copy->self->readback_,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

Status ContactShadowPass::read_back(Span<f32> out) const noexcept {
    if (device_ == nullptr || readback_.is_null()) {
        return fail(ErrorCode::Unavailable,
                    "contact shadows: created without `readback`, so there is nothing to read");
    }
    const usize pixels = static_cast<usize>(desc_.width) * desc_.height;
    if (out.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "contact shadows: the readback span is not the view");
    }
    const auto* texels = static_cast<const u8*>(device_->buffer_mapped_pointer(readback_));
    if (texels == nullptr) {
        return fail(ErrorCode::Internal, "contact shadows: the readback buffer is not mapped");
    }
    for (usize pixel = 0; pixel < pixels; ++pixel) {
        out[pixel] = static_cast<f32>(texels[pixel * 4U]) / 255.0F;
    }
    return ok();
}

}  // namespace cy::rendering::contact_shadows
