// SPDX-License-Identifier: MIT
// Native Metal compiler evidence belongs to the integration tier: even a tiny MSL module invokes
// Apple's driver compiler and deliberately exceeds the one-millisecond unit-case budget.

#include <cy/test/test.h>

#include <cy/backends/rhi-metal/backend.h>
#include <cy/backends/rhi/backend.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/system_allocator.h>

#include <cstdio>

namespace {

class Fixture {
public:
    Fixture() noexcept : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)) {
        (void)cy::rhi::metal::register_metal_backend();
        cy::rhi::DeviceDescription description;
        description.application_name = "integration.rhi_metal_shader";
        description.enable_validation = true;
        device_ = cy::rhi::create_device(allocator_, cy::rhi::metal::kMetalBackendName, description,
                                         selection_);
    }

    ~Fixture() {
        if (device_) {
            (void)(*device_)->wait_idle();
            cy::rhi::destroy_device(allocator_, *device_);
        }
    }

    [[nodiscard]] bool ok() const noexcept { return device_.has_value(); }
    [[nodiscard]] cy::rhi::Device& device() const noexcept { return **device_; }

private:
    cy::Allocator& allocator_;
    cy::rhi::BackendSelection selection_{};
    cy::Expected<cy::rhi::Device*, cy::Error> device_ =
        cy::fail(cy::ErrorCode::Unavailable, "not created");
};

}  // namespace

CY_TEST_CASE("Metal compiles MSL and rejects a shader that the driver cannot compile") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::Device& device = fixture.device();

    constexpr char source[] = R"msl(
#include <metal_stdlib>
using namespace metal;
vertex float4 metal_test_vertex(uint vertex_id [[vertex_id]]) {
    const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    return float4(positions[vertex_id], 0.0, 1.0);
}
)msl";
    cy::rhi::ShaderModuleDescription description;
    description.name = "native MSL vertex module";
    description.stage = cy::rhi::ShaderStage::Vertex;
    description.native = cy::Span<const cy::u8>(
        reinterpret_cast<const cy::u8*>(source), sizeof(source) - 1);
    description.native_format = cy::rhi::ShaderFormat::Msl;
    description.entry_point = "metal_test_vertex";

    const auto shader = device.create_shader_module(description);
    CY_REQUIRE(shader);
    device.destroy_shader_module(*shader);

    constexpr char invalid_source[] = "vertex this is deliberately not valid MSL";
    description.name = "invalid native MSL";
    description.native = cy::Span<const cy::u8>(
        reinterpret_cast<const cy::u8*>(invalid_source), sizeof(invalid_source) - 1);
    const cy::u64 errors_before = device.statistics().validation_errors;
    CY_CHECK_FALSE(device.create_shader_module(description));
    CY_CHECK_EQ(device.statistics().validation_errors, errors_before + 1);
}

CY_TEST_CASE("Metal creates graphics and compute pipelines with function constants") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::Device& device = fixture.device();

    constexpr char source[] = R"msl(
#include <metal_stdlib>
using namespace metal;
constant uint test_value [[function_constant(0)]];
vertex float4 pipeline_vertex(uint vertex_id [[vertex_id]]) {
    const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    return float4(positions[vertex_id], 0.0, 1.0);
}
fragment float4 pipeline_fragment() {
    return float4(float(test_value) / 255.0, 0.25, 0.5, 1.0);
}
kernel void pipeline_compute(uint index [[thread_position_in_grid]]) {
    if (index == test_value) { return; }
}
)msl";
    const cy::Span<const cy::u8> bytes(reinterpret_cast<const cy::u8*>(source),
                                       sizeof(source) - 1);
    auto create_shader = [&](cy::rhi::ShaderStage stage, const char* entry) {
        cy::rhi::ShaderModuleDescription description;
        description.name = entry;
        description.stage = stage;
        description.native = bytes;
        description.native_format = cy::rhi::ShaderFormat::Msl;
        description.entry_point = entry;
        return device.create_shader_module(description);
    };

    const auto vertex = create_shader(cy::rhi::ShaderStage::Vertex, "pipeline_vertex");
    const auto fragment = create_shader(cy::rhi::ShaderStage::Fragment, "pipeline_fragment");
    const auto compute = create_shader(cy::rhi::ShaderStage::Compute, "pipeline_compute");
    CY_REQUIRE(vertex);
    CY_REQUIRE(fragment);
    CY_REQUIRE(compute);

    cy::rhi::PipelineLayoutDescription layout_description;
    layout_description.name = "native empty layout";
    const auto layout = device.create_pipeline_layout(layout_description);
    CY_REQUIRE(layout);

    const cy::rhi::SpecializationConstant specialization{.id = 0, .value = 64};
    const cy::rhi::ColorAttachmentState color{.format = cy::rhi::Format::Rgba8Unorm};
    cy::rhi::GraphicsPipelineDescription graphics_description;
    graphics_description.name = "native graphics pipeline";
    graphics_description.layout = *layout;
    graphics_description.vertex_shader = *vertex;
    graphics_description.fragment_shader = *fragment;
    graphics_description.specialization = {&specialization, 1};
    graphics_description.color_attachments = {&color, 1};
    const auto graphics = device.create_graphics_pipeline(graphics_description);
    CY_REQUIRE(graphics);

    cy::rhi::ComputePipelineDescription compute_description;
    compute_description.name = "native compute pipeline";
    compute_description.layout = *layout;
    compute_description.shader = *compute;
    compute_description.specialization = {&specialization, 1};
    const auto compute_pipeline = device.create_compute_pipeline(compute_description);
    CY_REQUIRE(compute_pipeline);
    CY_CHECK_EQ(device.statistics().pipeline_cache_misses, 2U);

    constexpr char cache_path[] = "/tmp/cy-m11d5-metal-pipelines.bin";
    (void)std::remove(cache_path);
    CY_REQUIRE(device.save_pipeline_cache(cache_path));
    CY_REQUIRE(device.load_pipeline_cache(cache_path));
    const auto warm_graphics = device.create_graphics_pipeline(graphics_description);
    const auto warm_compute = device.create_compute_pipeline(compute_description);
    CY_REQUIRE(warm_graphics);
    CY_REQUIRE(warm_compute);
    CY_CHECK_EQ(device.statistics().pipeline_cache_hits, 2U);
    device.destroy_compute_pipeline(*warm_compute);
    device.destroy_graphics_pipeline(*warm_graphics);
    (void)std::remove(cache_path);

    device.destroy_compute_pipeline(*compute_pipeline);
    device.destroy_graphics_pipeline(*graphics);
    device.destroy_pipeline_layout(*layout);
    device.destroy_shader_module(*compute);
    device.destroy_shader_module(*fragment);
    device.destroy_shader_module(*vertex);
}

CY_TEST_CASE("Metal draws a native triangle and reads the rendered pixel back") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::Device& device = fixture.device();

    constexpr char source[] = R"msl(
#include <metal_stdlib>
using namespace metal;
vertex float4 readback_vertex(uint vertex_id [[vertex_id]]) {
    const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    return float4(positions[vertex_id], 0.0, 1.0);
}
fragment float4 readback_fragment() { return float4(1.0, 0.0, 0.0, 1.0); }
)msl";
    const cy::Span<const cy::u8> code(reinterpret_cast<const cy::u8*>(source),
                                      sizeof(source) - 1);
    auto create_shader = [&](cy::rhi::ShaderStage stage, const char* entry) {
        cy::rhi::ShaderModuleDescription description;
        description.name = entry;
        description.stage = stage;
        description.native = code;
        description.native_format = cy::rhi::ShaderFormat::Msl;
        description.entry_point = entry;
        return device.create_shader_module(description);
    };
    const auto vertex = create_shader(cy::rhi::ShaderStage::Vertex, "readback_vertex");
    const auto fragment = create_shader(cy::rhi::ShaderStage::Fragment, "readback_fragment");
    CY_REQUIRE(vertex);
    CY_REQUIRE(fragment);

    cy::rhi::PipelineLayoutDescription layout_description;
    const auto layout = device.create_pipeline_layout(layout_description);
    CY_REQUIRE(layout);
    const cy::rhi::ColorAttachmentState color_state{.format = cy::rhi::Format::Rgba8Unorm};
    cy::rhi::GraphicsPipelineDescription pipeline_description;
    pipeline_description.name = "readback pipeline";
    pipeline_description.layout = *layout;
    pipeline_description.vertex_shader = *vertex;
    pipeline_description.fragment_shader = *fragment;
    pipeline_description.rasterisation.cull_mode = cy::rhi::CullMode::None;
    pipeline_description.color_attachments = {&color_state, 1};
    const auto pipeline = device.create_graphics_pipeline(pipeline_description);
    CY_REQUIRE(pipeline);

    cy::rhi::TextureDescription texture_description;
    texture_description.name = "readback color target";
    texture_description.format = cy::rhi::Format::Rgba8Unorm;
    texture_description.extent = {4, 4, 1};
    texture_description.usage = cy::rhi::TextureUsage::ColorAttachment |
                                cy::rhi::TextureUsage::TransferSource;
    const auto texture = device.create_texture(texture_description);
    CY_REQUIRE(texture);
    cy::rhi::TextureViewDescription view_description;
    view_description.texture = *texture;
    const auto view = device.create_texture_view(view_description);
    CY_REQUIRE(view);

    cy::rhi::BufferDescription readback_description;
    readback_description.name = "rendered pixel readback";
    readback_description.size = 4 * 4 * 4;
    readback_description.usage = cy::rhi::BufferUsage::TransferDestination;
    readback_description.memory = cy::rhi::MemoryUse::Readback;
    const auto readback = device.create_buffer(readback_description);
    CY_REQUIRE(readback);

    const auto command_handle =
        device.acquire_command_buffer(cy::rhi::QueueKind::Graphics, false);
    CY_REQUIRE(command_handle);
    CY_REQUIRE(device.begin_command_buffer(*command_handle));
    cy::rhi::CommandBuffer* commands = device.command_buffer(*command_handle);
    CY_REQUIRE(commands != nullptr);

    cy::rhi::RenderAttachment attachment;
    attachment.view = *view;
    attachment.load = cy::rhi::LoadOp::Clear;
    attachment.store = cy::rhi::StoreOp::Store;
    attachment.clear.color[0] = 0.0F;
    attachment.clear.color[1] = 1.0F;
    attachment.clear.color[2] = 0.0F;
    attachment.clear.color[3] = 1.0F;
    cy::rhi::RenderingInfo rendering;
    rendering.render_area = {0, 0, 4, 4};
    rendering.color_attachments = {&attachment, 1};
    commands->begin_rendering(rendering);
    commands->set_viewport({0.0F, 0.0F, 4.0F, 4.0F, 0.0F, 1.0F});
    commands->set_scissor({0, 0, 4, 4});
    commands->bind_graphics_pipeline(*pipeline);
    commands->draw(3, 1, 0, 0);
    commands->end_rendering();

    cy::rhi::BufferTextureCopy copy;
    copy.texture_extent = {4, 4, 1};
    commands->copy_texture_to_buffer(*texture, *readback, {&copy, 1});
    CY_REQUIRE(device.end_command_buffer(*command_handle));

    cy::rhi::SubmitInfo submit;
    submit.command_buffers = {&*command_handle, 1};
    const auto signal = device.submit(submit);
    CY_REQUIRE(signal);
    CY_REQUIRE(device.wait_timeline(cy::rhi::QueueKind::Graphics, *signal, 5'000'000'000ULL));

    const auto* pixels = static_cast<const cy::u8*>(device.buffer_mapped_pointer(*readback));
    CY_REQUIRE(pixels != nullptr);
    CY_CHECK_EQ(pixels[0], 0xFFU);
    CY_CHECK_EQ(pixels[1], 0x00U);
    CY_CHECK_EQ(pixels[2], 0x00U);
    CY_CHECK_EQ(pixels[3], 0xFFU);

    device.destroy_buffer(*readback);
    device.destroy_texture_view(*view);
    device.destroy_texture(*texture);
    device.destroy_graphics_pipeline(*pipeline);
    device.destroy_pipeline_layout(*layout);
    device.destroy_shader_module(*fragment);
    device.destroy_shader_module(*vertex);
}

CY_TEST_CASE("Metal Tier 2 argument buffers are shader-readable") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::Device& device = fixture.device();
    CY_REQUIRE(device.capabilities().has(cy::rhi::Capability::Bindless));

    constexpr char source[] = R"msl(
#include <metal_stdlib>
using namespace metal;
struct ArgumentTable {
    array<texture2d<float>, 16384> textures [[id(0)]];
    sampler texture_sampler [[id(16384)]];
};
struct OutputTable {
    device uint* output [[id(0)]];
};
kernel void read_argument_buffer(constant ArgumentTable& table [[buffer(0)]],
                                 constant OutputTable& result [[buffer(1)]],
                                 uint3 local_id [[thread_position_in_threadgroup]]) {
    const uint linear = local_id.x + 8 * (local_id.y + 4 * local_id.z);
    if (linear == 0) {
        const float4 color = table.textures[0].sample(table.texture_sampler, float2(0.5));
        result.output[0] = color.r > 0.9 && color.g < 0.1 && color.b < 0.1
                               ? 0x00C0FFEEu
                               : 0xDEADBEEFu;
    } else {
        result.output[linear] = linear + 1;
    }
}
)msl";
    cy::rhi::ShaderModuleDescription shader_description;
    shader_description.name = "argument buffer reader";
    shader_description.stage = cy::rhi::ShaderStage::Compute;
    shader_description.native = {reinterpret_cast<const cy::u8*>(source), sizeof(source) - 1};
    shader_description.native_format = cy::rhi::ShaderFormat::Msl;
    shader_description.entry_point = "read_argument_buffer";
    const auto shader = device.create_shader_module(shader_description);
    CY_REQUIRE(shader);

    const cy::rhi::DescriptorBinding bindings[] = {{
        .binding = 3,
        .kind = cy::rhi::DescriptorKind::StorageBuffer,
        .count = 1,
        .stages = cy::rhi::ShaderStage::Compute,
    }};
    cy::rhi::DescriptorSetLayoutDescription set_layout_description;
    set_layout_description.name = "argument buffer test set";
    set_layout_description.bindings = bindings;
    const auto output_set_layout = device.create_descriptor_set_layout(set_layout_description);
    CY_REQUIRE(output_set_layout);
    const auto output_set = device.allocate_descriptor_set(*output_set_layout, false);
    CY_REQUIRE(output_set);

    cy::rhi::PipelineLayoutDescription pipeline_layout_description;
    pipeline_layout_description.name = "argument buffer test layout";
    const cy::rhi::DescriptorSetLayoutHandle pipeline_sets[] = {
        device.global_texture_table_layout(), *output_set_layout};
    CY_REQUIRE_FALSE(pipeline_sets[0].is_null());
    CY_REQUIRE_FALSE(device.global_texture_table().is_null());
    pipeline_layout_description.set_layouts = pipeline_sets;
    const auto pipeline_layout = device.create_pipeline_layout(pipeline_layout_description);
    CY_REQUIRE(pipeline_layout);
    cy::rhi::ComputePipelineDescription pipeline_description;
    pipeline_description.name = "argument buffer test pipeline";
    pipeline_description.layout = *pipeline_layout;
    pipeline_description.shader = *shader;
    pipeline_description.workgroup_size[0] = 8;
    pipeline_description.workgroup_size[1] = 4;
    pipeline_description.workgroup_size[2] = 2;
    const auto pipeline = device.create_compute_pipeline(pipeline_description);
    CY_REQUIRE(pipeline);

    cy::rhi::BufferDescription upload_description;
    upload_description.name = "argument buffer texture upload";
    upload_description.size = 256;
    upload_description.usage = cy::rhi::BufferUsage::TransferSource;
    upload_description.memory = cy::rhi::MemoryUse::Upload;
    const auto upload = device.create_buffer(upload_description);
    CY_REQUIRE(upload);
    auto* upload_bytes = static_cast<cy::u8*>(device.buffer_mapped_pointer(*upload));
    CY_REQUIRE(upload_bytes != nullptr);
    upload_bytes[0] = 0xFF;
    upload_bytes[1] = 0;
    upload_bytes[2] = 0;
    upload_bytes[3] = 0xFF;

    cy::rhi::TextureDescription texture_description;
    texture_description.name = "argument buffer sampled texture";
    texture_description.format = cy::rhi::Format::Rgba8Unorm;
    texture_description.extent = {1, 1, 1};
    texture_description.usage = cy::rhi::TextureUsage::Sampled |
                                cy::rhi::TextureUsage::TransferDestination;
    const auto texture = device.create_texture(texture_description);
    CY_REQUIRE(texture);
    cy::rhi::TextureViewDescription view_description;
    view_description.name = "argument buffer sampled view";
    view_description.texture = *texture;
    const auto view = device.create_texture_view(view_description);
    CY_REQUIRE(view);

    cy::rhi::SamplerDescription sampler_description;
    sampler_description.name = "argument buffer sampler";
    sampler_description.min_filter = cy::rhi::Filter::Nearest;
    sampler_description.mag_filter = cy::rhi::Filter::Nearest;
    const auto sampler = device.create_sampler(sampler_description);
    CY_REQUIRE(sampler);

    cy::rhi::BufferDescription output_description;
    output_description.name = "argument buffer result";
    output_description.size = 64 * sizeof(cy::u32);
    output_description.usage = cy::rhi::BufferUsage::Storage;
    output_description.memory = cy::rhi::MemoryUse::HostVisibleDeviceLocal;
    const auto output = device.create_buffer(output_description);
    CY_REQUIRE(output);
    *static_cast<cy::u32*>(device.buffer_mapped_pointer(*output)) = 0;

    const cy::rhi::DescriptorWrite output_write{
        .binding = 3, .kind = cy::rhi::DescriptorKind::StorageBuffer, .buffer = *output};
    CY_REQUIRE(device.update_descriptor_set(*output_set, {&output_write, 1}));
    const cy::rhi::BindlessIndex texture_index = device.bind_texture_globally(*view, *sampler);
    CY_REQUIRE_EQ(texture_index, 0U);

    const auto command = device.acquire_command_buffer(cy::rhi::QueueKind::Graphics, false);
    CY_REQUIRE(command);
    CY_REQUIRE(device.begin_command_buffer(*command));
    cy::rhi::CommandBuffer* commands = device.command_buffer(*command);
    CY_REQUIRE(commands != nullptr);
    cy::rhi::BufferTextureCopy upload_copy;
    upload_copy.buffer_row_length = 64;
    upload_copy.texture_extent = {1, 1, 1};
    commands->copy_buffer_to_texture(*upload, *texture, {&upload_copy, 1});
    commands->bind_compute_pipeline(*pipeline);
    const cy::rhi::DescriptorSetHandle descriptor_sets[] = {
        device.global_texture_table(), *output_set};
    commands->bind_descriptor_sets(*pipeline_layout, 0, descriptor_sets);
    commands->dispatch(1, 1, 1);
    CY_REQUIRE(device.end_command_buffer(*command));
    cy::rhi::SubmitInfo submit;
    submit.command_buffers = {&*command, 1};
    const auto signal = device.submit(submit);
    CY_REQUIRE(signal);
    CY_REQUIRE(device.wait_timeline(cy::rhi::QueueKind::Graphics, *signal,
                                    5'000'000'000ULL));
    CY_CHECK_EQ(*static_cast<const cy::u32*>(device.buffer_mapped_pointer(*output)), 0x00C0FFEEU);
    CY_CHECK_EQ(static_cast<const cy::u32*>(device.buffer_mapped_pointer(*output))[63], 64U);

    device.release_bindless_index(texture_index);
    device.destroy_buffer(*output);
    device.destroy_sampler(*sampler);
    device.destroy_texture_view(*view);
    device.destroy_texture(*texture);
    device.destroy_buffer(*upload);
    device.destroy_compute_pipeline(*pipeline);
    device.destroy_pipeline_layout(*pipeline_layout);
    device.destroy_descriptor_set_layout(*output_set_layout);
    device.destroy_shader_module(*shader);
}

CY_TEST_CASE("Metal shared events drive timelines, fences, and binary submissions") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::Device& device = fixture.device();

    const auto fence = device.create_fence(false);
    const auto semaphore = device.create_semaphore();
    CY_REQUIRE(fence);
    CY_REQUIRE(semaphore);
    CY_CHECK_FALSE(device.fence_signalled(*fence));

    const auto producer = device.acquire_command_buffer(cy::rhi::QueueKind::Graphics, false);
    CY_REQUIRE(producer);
    CY_REQUIRE(device.begin_command_buffer(*producer));
    CY_REQUIRE(device.end_command_buffer(*producer));
    cy::rhi::SubmitInfo produce;
    produce.command_buffers = {&*producer, 1};
    produce.signal_binary = *semaphore;
    produce.signal_fence = *fence;
    const auto produced = device.submit(produce);
    CY_REQUIRE(produced);
    CY_REQUIRE(device.wait_fence(*fence, 5'000'000'000ULL));
    CY_CHECK(device.fence_signalled(*fence));
    CY_CHECK_EQ(device.timeline_value(cy::rhi::QueueKind::Graphics), *produced);

    CY_REQUIRE(device.reset_fence(*fence));
    CY_CHECK_FALSE(device.fence_signalled(*fence));

    const auto consumer = device.acquire_command_buffer(cy::rhi::QueueKind::Graphics, false);
    CY_REQUIRE(consumer);
    CY_REQUIRE(device.begin_command_buffer(*consumer));
    CY_REQUIRE(device.end_command_buffer(*consumer));
    cy::rhi::SubmitInfo consume;
    consume.command_buffers = {&*consumer, 1};
    consume.wait_binary = *semaphore;
    consume.signal_fence = *fence;
    const auto consumed = device.submit(consume);
    CY_REQUIRE(consumed);
    CY_REQUIRE(device.wait_timeline(cy::rhi::QueueKind::Graphics, *consumed,
                                    5'000'000'000ULL));
    CY_CHECK(device.fence_signalled(*fence));
    CY_CHECK_EQ(device.statistics().semaphore_waits, 1U);

    device.destroy_semaphore(*semaphore);
    device.destroy_fence(*fence);
}

CY_TEST_CASE("Metal timestamp queries resolve GPU counter samples") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::Device& device = fixture.device();

    cy::rhi::QueryPoolDescription query_description;
    query_description.name = "native timestamp samples";
    query_description.kind = cy::rhi::QueryKind::Timestamp;
    query_description.count = 2;
    const auto queries = device.create_query_pool(query_description);
    CY_REQUIRE(queries);

    cy::rhi::BufferDescription buffer_description;
    buffer_description.name = "timestamp copy buffer";
    buffer_description.size = 16;
    buffer_description.usage = cy::rhi::BufferUsage::TransferSource |
                               cy::rhi::BufferUsage::TransferDestination;
    buffer_description.memory = cy::rhi::MemoryUse::HostVisibleDeviceLocal;
    const auto source = device.create_buffer(buffer_description);
    const auto destination = device.create_buffer(buffer_description);
    CY_REQUIRE(source);
    CY_REQUIRE(destination);

    const auto command = device.acquire_command_buffer(cy::rhi::QueueKind::Graphics, false);
    CY_REQUIRE(command);
    CY_REQUIRE(device.begin_command_buffer(*command));
    cy::rhi::CommandBuffer* commands = device.command_buffer(*command);
    CY_REQUIRE(commands != nullptr);
    commands->reset_queries(*queries, 0, 2);
    commands->write_timestamp(*queries, 0);
    const cy::rhi::BufferCopy copy{.size = 16};
    commands->copy_buffer(*source, *destination, {&copy, 1});
    commands->write_timestamp(*queries, 1);
    CY_REQUIRE(device.end_command_buffer(*command));
    cy::rhi::SubmitInfo submit;
    submit.command_buffers = {&*command, 1};
    const auto signal = device.submit(submit);
    CY_REQUIRE(signal);
    CY_REQUIRE(device.wait_timeline(cy::rhi::QueueKind::Graphics, *signal,
                                    5'000'000'000ULL));

    cy::u64 timestamps[2]{};
    const auto read = device.read_query_results(*queries, 0, 2, timestamps);
    CY_REQUIRE(read);
    CY_CHECK_EQ(*read, 2U);
    CY_CHECK_NE(timestamps[0], ~0ULL);
    CY_CHECK_NE(timestamps[1], ~0ULL);
    CY_CHECK(timestamps[1] >= timestamps[0]);

    device.destroy_buffer(*destination);
    device.destroy_buffer(*source);
    device.destroy_query_pool(*queries);
}
