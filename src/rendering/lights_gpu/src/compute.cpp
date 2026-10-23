// SPDX-License-Identifier: MIT
#include <cy/rendering/lights_gpu/detail/compute.h>

#include <cy/backends/rhi/validation.h>

namespace cy::rendering::lights_gpu::detail {

namespace {
constexpr u32 kMaxBindings = 32;
}  // namespace

Status ComputePipeline::create(rhi::Device& device, const ComputeProgram& program) noexcept {
    if (program.bindings.size() > kMaxBindings) {
        return fail(ErrorCode::InvalidArgument, "lights_gpu: a pass has more bindings than 32");
    }
    rhi::ShaderModuleBundle bundle;
    bundle.spirv = program.spirv;
    bundle.msl = program.msl;
    bundle.msl_entry_point = program.msl_entry;
    bundle.spirv_entry_point = "main";
    rhi::ValidationMessage message;
    auto module = rhi::select_shader_module(bundle, device.capabilities().native_shader_format(),
                                            program.name, rhi::ShaderStage::Compute, message);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    Expected<rhi::ShaderModuleHandle, Error> shader = device.create_shader_module(*module);
    if (!shader.has_value()) {
        return make_unexpected(shader.error());
    }
    shader_ = *shader;

    rhi::DescriptorBinding bindings[kMaxBindings] = {};
    const auto count = static_cast<u32>(program.bindings.size());
    for (u32 index = 0; index < count; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = program.bindings[index];
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = program.name;
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, count);
    Expected<rhi::DescriptorSetLayoutHandle, Error> made_set =
        device.create_descriptor_set_layout(set_layout);
    if (!made_set.has_value()) {
        destroy(device);
        return make_unexpected(made_set.error());
    }
    set_layout_ = *made_set;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0, program.push_bytes};
    rhi::PipelineLayoutDescription layout;
    layout.name = program.name;
    layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    if (program.push_bytes > 0) {
        layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    }
    Expected<rhi::PipelineLayoutHandle, Error> made_layout = device.create_pipeline_layout(layout);
    if (!made_layout.has_value()) {
        destroy(device);
        return make_unexpected(made_layout.error());
    }
    layout_ = *made_layout;

    rhi::ComputePipelineDescription pipeline;
    pipeline.name = program.name;
    pipeline.layout = layout_;
    pipeline.shader = shader_;
    pipeline.workgroup_size[0] = program.workgroup[0];
    pipeline.workgroup_size[1] = program.workgroup[1];
    pipeline.workgroup_size[2] = program.workgroup[2];
    Expected<rhi::ComputePipelineHandle, Error> made = device.create_compute_pipeline(pipeline);
    if (!made.has_value()) {
        destroy(device);
        return make_unexpected(made.error());
    }
    pipeline_ = *made;
    return ok();
}

void ComputePipeline::destroy(rhi::Device& device) noexcept {
    if (!pipeline_.is_null()) {
        device.destroy_compute_pipeline(pipeline_);
    }
    if (!layout_.is_null()) {
        device.destroy_pipeline_layout(layout_);
    }
    if (!set_layout_.is_null()) {
        device.destroy_descriptor_set_layout(set_layout_);
    }
    if (!shader_.is_null()) {
        device.destroy_shader_module(shader_);
    }
    pipeline_ = {};
    layout_ = {};
    set_layout_ = {};
    shader_ = {};
}

Expected<rhi::DescriptorSetHandle, Error> ComputePipeline::set(
    rhi::Device& device, Span<const rhi::DescriptorWrite> writes) const noexcept {
    Expected<rhi::DescriptorSetHandle, Error> set =
        device.allocate_descriptor_set(set_layout_, true);
    if (!set.has_value()) {
        return set;
    }
    if (Status written = device.update_descriptor_set(*set, writes); !written) {
        return make_unexpected(written.error());
    }
    return set;
}

bool compute_supported(const rhi::Device& device) noexcept {
    const rhi::ShaderFormat format = device.capabilities().native_shader_format();
    return device.capabilities().has(rhi::Capability::ComputeShaders) &&
           (format == rhi::ShaderFormat::Spirv || format == rhi::ShaderFormat::Msl);
}

Expected<rhi::BufferHandle, Error> host_buffer(rhi::Device& device, const char* name, u64 size,
                                               rhi::BufferUsage usage) noexcept {
    rhi::BufferDescription description;
    description.name = name;
    description.size = size > 0 ? size : 16U;
    description.usage = usage;
    description.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(description);
    if (!buffer.has_value()) {
        return buffer;
    }
    if (device.buffer_mapped_pointer(*buffer) == nullptr) {
        device.destroy_buffer(*buffer);
        return fail(ErrorCode::Internal, "lights_gpu: a host-visible buffer is not mapped");
    }
    return buffer;
}

Expected<rhi::BufferHandle, Error> device_buffer(rhi::Device& device, const char* name, u64 size,
                                                 rhi::BufferUsage usage) noexcept {
    rhi::BufferDescription description;
    description.name = name;
    description.size = size > 0 ? size : 16U;
    description.usage = usage;
    description.memory = rhi::MemoryUse::DeviceLocal;
    return device.create_buffer(description);
}

rhi::DescriptorWrite storage_write(u32 binding, rhi::BufferHandle buffer) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind = rhi::DescriptorKind::StorageBuffer;
    write.buffer = buffer;
    write.buffer_offset = 0;
    write.buffer_range = 0;  // the rest of the buffer
    return write;
}

rhi::DescriptorWrite texture_write(u32 binding, rhi::TextureViewHandle view,
                                   bool storage) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind =
        storage ? rhi::DescriptorKind::StorageTexture : rhi::DescriptorKind::SampledTexture;
    write.texture_view = view;
    write.use = storage ? rhi::ImageUse::Storage : rhi::ImageUse::SampledRead;
    return write;
}

}  // namespace cy::rendering::lights_gpu::detail
