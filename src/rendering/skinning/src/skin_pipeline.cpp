#include "skin_pipeline.h"

#include <cy/backends/rhi/validation.h>
#include <cy/servers/render/geometry/skin_dispatch.h>

#include "skin_msl.h"
#include "skin_spirv.h"

namespace cy::rendering::skinning::detail {

bool skin_supported(const rhi::Device& device) noexcept {
    const rhi::ShaderFormat format = device.capabilities().native_shader_format();
    return device.capabilities().has(rhi::Capability::ComputeShaders) &&
           (format == rhi::ShaderFormat::Spirv || format == rhi::ShaderFormat::Msl);
}

Status skin_unsupported(const rhi::Device& device) noexcept {
    if (device.capabilities().has(rhi::Capability::ComputeShaders)) {
        return fail(ErrorCode::Unsupported,
                    "skinning package has no shader for the device's native format");
    }
    return fail(ErrorCode::Unsupported,
                "skinning needs Capability::ComputeShaders; cpu_reference_skin computes the same "
                "answer and is what a device without one would have to run");
}

Status SkinPipeline::create(rhi::Device& device) noexcept {
    rhi::ShaderModuleBundle bundle;
    bundle.spirv = {kSkinVerticesSpirv, sizeof(kSkinVerticesSpirv) / sizeof(u32)};
    bundle.msl = {reinterpret_cast<const u8*>(kSkinVerticesMsl), sizeof(kSkinVerticesMsl) - 1};
    bundle.spirv_entry_point = "main";
    bundle.msl_entry_point = "skin_vertices";
    rhi::ValidationMessage message;
    auto module = rhi::select_shader_module(bundle, device.capabilities().native_shader_format(),
                                            "skin vertices", rhi::ShaderStage::Compute, message);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    Expected<rhi::ShaderModuleHandle, Error> created = device.create_shader_module(*module);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    shader = *created;

    rhi::DescriptorBinding bindings[kBindingCount] = {};
    for (u32 index = 0; index < kBindingCount; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_description;
    set_description.name = "skin set";
    set_description.bindings = Span<const rhi::DescriptorBinding>(bindings, kBindingCount);
    Expected<rhi::DescriptorSetLayoutHandle, Error> made_set =
        device.create_descriptor_set_layout(set_description);
    if (!made_set.has_value()) {
        return make_unexpected(made_set.error());
    }
    set_layout = *made_set;

    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0,
                                       sizeof(render::geometry::GpuSkinConstants)};
    rhi::PipelineLayoutDescription layout_description;
    layout_description.name = "skin layout";
    layout_description.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout, 1);
    layout_description.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> made_layout =
        device.create_pipeline_layout(layout_description);
    if (!made_layout.has_value()) {
        return make_unexpected(made_layout.error());
    }
    layout = *made_layout;

    rhi::ComputePipelineDescription description;
    description.name = "skin vertices";
    description.layout = layout;
    description.shader = shader;
    description.workgroup_size[0] = kSkinGroupSize;
    Expected<rhi::ComputePipelineHandle, Error> made_pipeline =
        device.create_compute_pipeline(description);
    if (!made_pipeline.has_value()) {
        return make_unexpected(made_pipeline.error());
    }
    pipeline = *made_pipeline;
    return ok();
}

void SkinPipeline::destroy(rhi::Device& device) noexcept {
    if (pipeline) {
        device.destroy_compute_pipeline(pipeline);
    }
    if (layout) {
        device.destroy_pipeline_layout(layout);
    }
    if (set_layout) {
        device.destroy_descriptor_set_layout(set_layout);
    }
    if (shader) {
        device.destroy_shader_module(shader);
    }
    *this = SkinPipeline{};
}

Status write_skin_set(rhi::Device& device, rhi::DescriptorSetHandle set,
                      const rhi::BufferHandle (&buffers)[kBindingCount]) noexcept {
    rhi::DescriptorWrite writes[kBindingCount] = {};
    for (u32 index = 0; index < kBindingCount; ++index) {
        writes[index].binding = index;
        writes[index].kind = rhi::DescriptorKind::StorageBuffer;
        writes[index].buffer = buffers[index];
        writes[index].buffer_range = 0;  // the rest of the buffer
    }
    return device.update_descriptor_set(set,
                                        Span<const rhi::DescriptorWrite>(writes, kBindingCount));
}

}  // namespace cy::rendering::skinning::detail
