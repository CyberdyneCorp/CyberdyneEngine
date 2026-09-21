// SPDX-License-Identifier: MIT
// Native Metal device conformance. This suite is declared only for an Apple build with Metal on;
// its result always prints which device and argument-buffer tier answered.

#include <cy/test/test.h>

#include <cy/backends/rhi-metal/backend.h>
#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/system_allocator.h>

#include <cstring>

namespace {

class Fixture {
public:
    Fixture() noexcept : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)) {
        (void)cy::rhi::metal::register_metal_backend();
        cy::rhi::DeviceDescription description;
        description.application_name = "integration.rhi_metal";
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
    [[nodiscard]] const cy::rhi::BackendSelection& selection() const noexcept { return selection_; }

private:
    cy::Allocator& allocator_;
    cy::rhi::BackendSelection selection_{};
    cy::Expected<cy::rhi::Device*, cy::Error> device_ =
        cy::fail(cy::ErrorCode::Unavailable, "not created");
};

}  // namespace

CY_TEST_CASE("the Metal device names the hardware and argument-buffer tier that answered") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());

    const cy::rhi::metal::MetalRuntimeInfo runtime = cy::rhi::metal::metal_runtime_info();
    CY_TEST_MESSAGE("Metal device: ", runtime.device_name, "; argument buffers: Tier ",
                    runtime.argument_buffer_tier,
                    "; Apple GPU family: ", runtime.apple_gpu_family ? 1 : 0);
    CY_CHECK(runtime.device_name[0] != '\0');
    CY_CHECK_EQ(runtime.argument_buffer_tier, 2U);
    CY_CHECK(runtime.apple_gpu_family);

    const cy::rhi::DeviceCapabilities& caps = fixture.device().capabilities();
    CY_CHECK_EQ(fixture.selection().kind, cy::rhi::BackendKind::Metal);
    CY_CHECK_EQ(caps.backend(), cy::rhi::BackendKind::Metal);
    CY_CHECK_EQ(std::strcmp(caps.device_name(), runtime.device_name), 0);
    CY_CHECK(caps.supports_gpu_driven());
    CY_CHECK_FALSE(caps.has(cy::rhi::Capability::ParallelPassRecording));
    CY_CHECK_FALSE(caps.needs_queue_ownership_transfer());
    CY_CHECK(fixture.device().native_handle() != nullptr);

    cy::rhi::ValidationMessage validation;
    CY_CHECK(cy::rhi::validate_device_limits(caps.limits(), validation));
}

CY_TEST_CASE("Metal upload and readback buffers expose shared unified memory") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::Device& device = fixture.device();

    cy::rhi::BufferDescription description;
    description.name = "native shared buffer";
    description.size = 256;
    description.usage =
        cy::rhi::BufferUsage::TransferSource | cy::rhi::BufferUsage::TransferDestination;
    description.memory = cy::rhi::MemoryUse::HostVisibleDeviceLocal;

    const cy::Expected<cy::rhi::BufferHandle, cy::Error> created =
        device.create_buffer(description);
    CY_REQUIRE(created);
    CY_CHECK(device.is_valid(*created));
    CY_REQUIRE(device.buffer_mapped_pointer(*created) != nullptr);
    std::memset(device.buffer_mapped_pointer(*created), 0x5A,
                static_cast<cy::usize>(description.size));
    CY_CHECK_EQ(static_cast<const cy::u8*>(device.buffer_mapped_pointer(*created))[127], 0x5AU);
    CY_REQUIRE(device.buffer_description(*created) != nullptr);
    CY_CHECK_EQ(device.buffer_description(*created)->size, description.size);

    device.destroy_buffer(*created);
    CY_CHECK_FALSE(device.is_valid(*created));
    CY_CHECK(device.buffer_mapped_pointer(*created) == nullptr);
}

CY_TEST_CASE("Metal textures and views preserve generational handle validity") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::Device& device = fixture.device();

    cy::rhi::TextureDescription description;
    description.name = "native color target";
    description.format = cy::rhi::Format::Rgba8Unorm;
    description.extent = {64, 64, 1};
    description.mip_levels = 2;
    description.usage = cy::rhi::TextureUsage::ColorAttachment | cy::rhi::TextureUsage::Sampled;

    const auto texture = device.create_texture(description);
    CY_REQUIRE(texture);
    CY_CHECK(device.is_valid(*texture));
    CY_REQUIRE(device.texture_description(*texture) != nullptr);
    CY_CHECK_EQ(device.texture_description(*texture)->mip_levels, 2U);

    cy::rhi::TextureViewDescription view_description;
    view_description.name = "native color view";
    view_description.texture = *texture;
    const auto view = device.create_texture_view(view_description);
    CY_REQUIRE(view);
    CY_CHECK(device.is_valid(*view));

    device.destroy_texture_view(*view);
    CY_CHECK_FALSE(device.is_valid(*view));
    device.destroy_texture(*texture);
    CY_CHECK_FALSE(device.is_valid(*texture));
}

CY_TEST_CASE("Metal memoryless attachments allocate in tile memory on Apple-family hardware") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::Device& device = fixture.device();

    // An imported-resource-only graph asks for no heap. MTLHeap rejects size zero, so this must be
    // handled before attempting allocation.
    CY_CHECK(device.reserve_transient_memory(0, cy::rhi::MemoryPoolClass{}));
    CY_CHECK_EQ(device.transient_pool_bytes(), 0U);

    cy::rhi::TextureDescription description;
    description.name = "memoryless depth";
    description.format = cy::rhi::Format::D32Sfloat;
    description.extent = {128, 128, 1};
    description.usage =
        cy::rhi::TextureUsage::DepthStencilAttachment | cy::rhi::TextureUsage::TransientAttachment;

    const auto texture = device.create_transient_texture(description);
    CY_REQUIRE(texture);
    const auto requirements = device.texture_memory_requirements(*texture);
    CY_REQUIRE(requirements);
    CY_CHECK_EQ(requirements->size, 0U);
    CY_CHECK_EQ(device.transient_pool_bytes(), 0U);

    cy::rhi::TextureViewDescription view_description;
    view_description.name = "memoryless depth view";
    view_description.texture = *texture;
    const auto view = device.create_texture_view(view_description);
    CY_REQUIRE(view);
    device.destroy_texture_view(*view);

    device.release_transient_resources();
    CY_CHECK_FALSE(device.is_valid(*texture));
}

CY_TEST_CASE("Metal placement heaps honor the render graph memory-pool meet") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::Device& device = fixture.device();

    cy::rhi::TextureDescription texture_description;
    texture_description.name = "aliased transient texture";
    texture_description.format = cy::rhi::Format::Rgba16Sfloat;
    texture_description.extent = {256, 256, 1};
    texture_description.usage =
        cy::rhi::TextureUsage::ColorAttachment | cy::rhi::TextureUsage::Sampled;
    const auto texture = device.create_transient_texture(texture_description);
    CY_REQUIRE(texture);

    cy::rhi::BufferDescription buffer_description;
    buffer_description.name = "aliased transient buffer";
    buffer_description.size = 512 * 1024;
    buffer_description.usage = cy::rhi::BufferUsage::Storage;
    const auto buffer = device.create_transient_buffer(buffer_description);
    CY_REQUIRE(buffer);

    const auto texture_memory = device.texture_memory_requirements(*texture);
    const auto buffer_memory = device.buffer_memory_requirements(*buffer);
    CY_REQUIRE(texture_memory);
    CY_REQUIRE(buffer_memory);
    const cy::rhi::MemoryPoolClass common =
        meet(texture_memory->pool_class, buffer_memory->pool_class);
    CY_CHECK_FALSE(common.empty());

    const cy::u64 required =
        texture_memory->size > buffer_memory->size ? texture_memory->size : buffer_memory->size;
    CY_REQUIRE(device.reserve_transient_memory(required, common));
    CY_CHECK_EQ(device.transient_pool_bytes(), required);
    CY_REQUIRE(device.bind_transient(*texture, 0));
    CY_REQUIRE(device.bind_transient(*buffer, 0));

    cy::rhi::TextureViewDescription view_description;
    view_description.texture = *texture;
    const auto view = device.create_texture_view(view_description);
    CY_REQUIRE(view);
    device.destroy_texture_view(*view);

    device.release_transient_resources();
    CY_CHECK_FALSE(device.is_valid(*texture));
    CY_CHECK_FALSE(device.is_valid(*buffer));
    CY_CHECK_EQ(device.memory_report()
                    .live_bytes[static_cast<cy::u32>(cy::rhi::GpuMemoryCategory::Transient)],
                required);
}

CY_TEST_CASE("Metal creates native sampler states and enforces the device limit") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::Device& device = fixture.device();

    cy::rhi::SamplerDescription description;
    description.name = "native comparison sampler";
    description.min_filter = cy::rhi::Filter::Nearest;
    description.mag_filter = cy::rhi::Filter::Linear;
    description.address_u = cy::rhi::AddressMode::ClampToBorder;
    description.max_anisotropy = 4.0F;
    description.compare_enable = true;
    description.compare_op = cy::rhi::CompareOp::GreaterOrEqual;

    const auto sampler = device.create_sampler(description);
    CY_REQUIRE(sampler);
    const cy::u64 freed_before = device.statistics().resources_freed;
    device.destroy_sampler(*sampler);
    CY_CHECK_EQ(device.statistics().resources_freed, freed_before + 1);

    description.max_anisotropy = device.capabilities().limits().max_sampler_anisotropy + 1.0F;
    CY_CHECK_FALSE(device.create_sampler(description));
}

CY_TEST_CASE("Metal recycles per-frame command and descriptor pools after GPU completion") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::Device& device = fixture.device();

    const cy::rhi::DescriptorBinding binding{
        .binding = 0,
        .kind = cy::rhi::DescriptorKind::StorageBuffer,
        .count = 1,
        .stages = cy::rhi::ShaderStage::Compute,
    };
    cy::rhi::DescriptorSetLayoutDescription layout_description;
    layout_description.bindings = {&binding, 1};
    const auto layout = device.create_descriptor_set_layout(layout_description);
    CY_REQUIRE(layout);

    const auto first_slot = device.begin_frame();
    CY_REQUIRE(first_slot);
    CY_CHECK_EQ(*first_slot, 0U);
    const auto set = device.allocate_descriptor_set(*layout, true);
    const auto command = device.acquire_command_buffer(cy::rhi::QueueKind::Graphics, false);
    CY_REQUIRE(set);
    CY_REQUIRE(command);
    CY_REQUIRE(device.begin_command_buffer(*command));
    CY_REQUIRE(device.end_command_buffer(*command));
    cy::rhi::SubmitInfo submit;
    submit.command_buffers = {&*command, 1};
    CY_REQUIRE(device.submit(submit));
    CY_REQUIRE(device.end_frame());

    for (cy::u32 frame = 1; frame < device.frames_in_flight(); ++frame) {
        CY_REQUIRE(device.begin_frame());
        CY_REQUIRE(device.end_frame());
    }
    const auto recycled_slot = device.begin_frame();
    CY_REQUIRE(recycled_slot);
    CY_CHECK_EQ(*recycled_slot, 0U);
    CY_CHECK(device.command_buffer(*command) == nullptr);
    CY_CHECK_FALSE(device.update_descriptor_set(*set, {}));

    device.destroy_descriptor_set_layout(*layout);
}
