#include <cy/pcg/gpu_executor.h>

#include "../shaders/gpu_conformance_embedded.h"

#include <cstring>

namespace cy::pcg::gpu {
namespace {

class Resources {
public:
    explicit Resources(rhi::Device& device) noexcept : device_(device) {}
    ~Resources() {
        if (!pipeline.is_null()) {
            device_.destroy_compute_pipeline(pipeline);
        }
        if (!shader.is_null()) {
            device_.destroy_shader_module(shader);
        }
        if (!pipeline_layout.is_null()) {
            device_.destroy_pipeline_layout(pipeline_layout);
        }
        if (!set_layout.is_null()) {
            device_.destroy_descriptor_set_layout(set_layout);
        }
        if (!output.is_null()) {
            device_.destroy_buffer(output);
        }
        if (!parameters.is_null()) {
            device_.destroy_buffer(parameters);
        }
    }

    rhi::BufferHandle parameters;
    rhi::BufferHandle output;
    rhi::DescriptorSetLayoutHandle set_layout;
    rhi::PipelineLayoutHandle pipeline_layout;
    rhi::ShaderModuleHandle shader;
    rhi::ComputePipelineHandle pipeline;
    rhi::DescriptorSetHandle descriptors;

private:
    rhi::Device& device_;
};

[[nodiscard]] Expected<rhi::ShaderModuleHandle, Error> create_shader(rhi::Device& device) noexcept {
    rhi::ShaderModuleDescription description;
    description.name = "pcg candidate conformance";
    description.stage = rhi::ShaderStage::Compute;
    if (device.capabilities().native_shader_format() == rhi::ShaderFormat::Msl) {
        description.native =
            Span<const u8>(reinterpret_cast<const u8*>(kCandidateMsl), sizeof(kCandidateMsl) - 1);
        description.native_format = rhi::ShaderFormat::Msl;
        description.entry_point = "pcgCandidates";
    } else {
        description.spirv = Span<const u32>(kCandidateSpirv);
        description.entry_point = "main";
    }
    return device.create_shader_module(description);
}

}  // namespace

Expected<AgreementReport, Error> run_candidate_agreement(rhi::Device& device,
                                                         const GpuCandidateParameters& parameters,
                                                         Span<GpuCandidate> output) noexcept {
    if (parameters.count == 0 || output.size() != parameters.count) {
        return fail(ErrorCode::InvalidArgument,
                    "GPU PCG agreement requires one output record per non-zero candidate");
    }
    if (!device.capabilities().has(rhi::Capability::ComputeShaders)) {
        return fail(ErrorCode::Unsupported, "GPU PCG agreement requires compute shaders");
    }

    Resources resources(device);
    rhi::BufferDescription parameters_description;
    parameters_description.name = "pcg candidate parameters";
    parameters_description.size = sizeof(GpuCandidateParameters);
    parameters_description.usage = rhi::BufferUsage::Uniform;
    parameters_description.memory = rhi::MemoryUse::HostVisibleDeviceLocal;
    auto parameter_buffer = device.create_buffer(parameters_description);
    if (!parameter_buffer) {
        return make_unexpected(parameter_buffer.error());
    }
    resources.parameters = *parameter_buffer;

    rhi::BufferDescription output_description;
    output_description.name = "pcg candidate output";
    output_description.size = output.size_bytes();
    output_description.usage = rhi::BufferUsage::Storage;
    output_description.memory = rhi::MemoryUse::HostVisibleDeviceLocal;
    auto output_buffer = device.create_buffer(output_description);
    if (!output_buffer) {
        return make_unexpected(output_buffer.error());
    }
    resources.output = *output_buffer;

    void* parameter_mapping = device.buffer_mapped_pointer(resources.parameters);
    void* output_mapping = device.buffer_mapped_pointer(resources.output);
    if (parameter_mapping == nullptr || output_mapping == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "GPU PCG agreement needs host-visible parameter and readback buffers");
    }
    std::memcpy(parameter_mapping, &parameters, sizeof(parameters));
    std::memset(output_mapping, 0, output.size_bytes());

    const rhi::DescriptorBinding bindings[] = {
        {.binding = 0,
         .kind = rhi::DescriptorKind::UniformBuffer,
         .count = 1,
         .stages = rhi::ShaderStage::Compute},
        {.binding = 1,
         .kind = rhi::DescriptorKind::StorageBuffer,
         .count = 1,
         .stages = rhi::ShaderStage::Compute},
    };
    rhi::DescriptorSetLayoutDescription set_description;
    set_description.name = "pcg candidate conformance";
    set_description.bindings = bindings;
    auto set_layout = device.create_descriptor_set_layout(set_description);
    if (!set_layout) {
        return make_unexpected(set_layout.error());
    }
    resources.set_layout = *set_layout;

    rhi::PipelineLayoutDescription layout_description;
    layout_description.name = "pcg candidate conformance";
    layout_description.set_layouts =
        Span<const rhi::DescriptorSetLayoutHandle>(&resources.set_layout, 1);
    auto pipeline_layout = device.create_pipeline_layout(layout_description);
    if (!pipeline_layout) {
        return make_unexpected(pipeline_layout.error());
    }
    resources.pipeline_layout = *pipeline_layout;

    auto descriptors = device.allocate_descriptor_set(resources.set_layout, false);
    if (!descriptors) {
        return make_unexpected(descriptors.error());
    }
    resources.descriptors = *descriptors;
    const rhi::DescriptorWrite writes[] = {
        {.binding = 0, .kind = rhi::DescriptorKind::UniformBuffer, .buffer = resources.parameters},
        {.binding = 1, .kind = rhi::DescriptorKind::StorageBuffer, .buffer = resources.output},
    };
    if (Status updated = device.update_descriptor_set(resources.descriptors, writes); !updated) {
        return make_unexpected(updated.error());
    }

    auto shader = create_shader(device);
    if (!shader) {
        return make_unexpected(shader.error());
    }
    resources.shader = *shader;
    rhi::ComputePipelineDescription pipeline_description;
    pipeline_description.name = "pcg candidate conformance";
    pipeline_description.layout = resources.pipeline_layout;
    pipeline_description.shader = resources.shader;
    pipeline_description.workgroup_size[0] = 64;
    auto pipeline = device.create_compute_pipeline(pipeline_description);
    if (!pipeline) {
        return make_unexpected(pipeline.error());
    }
    resources.pipeline = *pipeline;

    auto command = device.acquire_command_buffer(rhi::QueueKind::Graphics, false);
    if (!command) {
        return make_unexpected(command.error());
    }
    if (Status begun = device.begin_command_buffer(*command); !begun) {
        return make_unexpected(begun.error());
    }
    rhi::CommandBuffer* commands = device.command_buffer(*command);
    if (commands == nullptr) {
        return fail(ErrorCode::Internal, "GPU PCG command buffer became stale");
    }
    commands->bind_compute_pipeline(resources.pipeline);
    commands->bind_descriptor_sets(resources.pipeline_layout, 0,
                                   Span<const rhi::DescriptorSetHandle>(&resources.descriptors, 1));
    commands->dispatch((parameters.count + 63U) / 64U, 1, 1);
    if (Status ended = device.end_command_buffer(*command); !ended) {
        return make_unexpected(ended.error());
    }
    rhi::SubmitInfo submit;
    submit.command_buffers = Span<const rhi::CommandBufferHandle>(&*command, 1);
    auto signal = device.submit(submit);
    if (!signal) {
        return make_unexpected(signal.error());
    }
    if (Status waited = device.wait_timeline(rhi::QueueKind::Graphics, *signal, 5'000'000'000ULL);
        !waited) {
        return make_unexpected(waited.error());
    }
    std::memcpy(output.data(), output_mapping, output.size_bytes());

    AgreementReport report;
    report.cpu = summarize_gpu_candidate_reference(parameters);
    report.device = summarize_gpu_candidates(parameters, output);
    report.all_records_equal = true;
    for (u32 index = 0; index < parameters.count; ++index) {
        if (gpu_candidate_at(parameters.seed, index, parameters.density_threshold) !=
            output[index]) {
            report.first_mismatch = index;
            report.all_records_equal = false;
            break;
        }
    }
    return report;
}

}  // namespace cy::pcg::gpu
