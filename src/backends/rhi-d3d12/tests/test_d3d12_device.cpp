// SPDX-License-Identifier: MIT

#include <cy/test/test.h>

#include <cy/backends/rhi-d3d12/backend.h>
#include <cy/backends/rhi/backend.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/system_allocator.h>

#include <cstring>

namespace {
class Fixture {
public:
    Fixture() noexcept : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)) {
        (void)cy::rhi::d3d12::register_d3d12_backend();
        cy::rhi::DeviceDescription description;
        description.application_name = "unit.rhi_d3d12";
        description.enable_validation = true;
        device_ = cy::rhi::create_device(allocator_, cy::rhi::d3d12::kD3D12BackendName, description,
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

CY_TEST_CASE("D3D12 Resource Heap Tier 1 partitions incompatible resources without a device") {
    using cy::rhi::d3d12::HeapResourceClass;
    using cy::rhi::d3d12::memory_pool_class;
    const cy::rhi::MemoryPoolClass tier1_buffer = memory_pool_class(1, HeapResourceClass::Buffer);
    const cy::rhi::MemoryPoolClass tier1_texture = memory_pool_class(1, HeapResourceClass::Texture);
    const cy::rhi::MemoryPoolClass tier1_target =
        memory_pool_class(1, HeapResourceClass::RenderTarget);
    CY_CHECK(meet(tier1_buffer, tier1_texture).empty());
    CY_CHECK(meet(tier1_texture, tier1_target).empty());

    const cy::rhi::MemoryPoolClass tier2_buffer = memory_pool_class(2, HeapResourceClass::Buffer);
    const cy::rhi::MemoryPoolClass tier2_target =
        memory_pool_class(2, HeapResourceClass::RenderTarget);
    CY_CHECK_FALSE(meet(tier2_buffer, tier2_target).empty());
}

CY_TEST_CASE("the D3D12 device names the adapter and class that answered") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    const cy::rhi::d3d12::AdapterIdentity identity = cy::rhi::d3d12::d3d12_runtime_identity();
    CY_TEST_MESSAGE("D3D12 adapter: ", identity.name, "; vendor: ", identity.vendor_id,
                    "; class: ", cy::rhi::d3d12::adapter_class_name(identity.classification),
                    "; DXGI software flag: ", identity.dxgi_software_flag ? 1 : 0,
                    "; resource heap tier: ", identity.resource_heap_tier);
    CY_CHECK(identity.name[0] != '\0');
    const bool known_resource_heap_tier =
        identity.resource_heap_tier == 1 || identity.resource_heap_tier == 2;
    CY_CHECK(known_resource_heap_tier);
    CY_CHECK_EQ(fixture.selection().kind, cy::rhi::BackendKind::D3D12);
    CY_CHECK_EQ(fixture.device().capabilities().backend(), cy::rhi::BackendKind::D3D12);
    CY_CHECK_EQ(std::strcmp(fixture.device().capabilities().device_name(), identity.name), 0);
    CY_CHECK_EQ(fixture.device().capabilities().native_shader_format(),
                cy::rhi::ShaderFormat::Dxil);
    CY_CHECK_FALSE(fixture.device().capabilities().needs_queue_ownership_transfer());
    CY_CHECK(fixture.device().native_handle() != nullptr);
}

CY_TEST_CASE("D3D12 host buffers map and stale handles are rejected") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::BufferDescription description;
    description.name = "D3D12 upload";
    description.size = 256;
    description.usage = cy::rhi::BufferUsage::TransferSource;
    description.memory = cy::rhi::MemoryUse::Upload;
    const auto buffer = fixture.device().create_buffer(description);
    CY_REQUIRE(buffer);
    CY_REQUIRE(fixture.device().buffer_mapped_pointer(*buffer) != nullptr);
    std::memset(fixture.device().buffer_mapped_pointer(*buffer), 0x5A, 256);
    CY_CHECK_EQ(static_cast<cy::u8*>(fixture.device().buffer_mapped_pointer(*buffer))[127], 0x5AU);
    fixture.device().destroy_buffer(*buffer);
    CY_CHECK_FALSE(fixture.device().is_valid(*buffer));
}

CY_TEST_CASE("D3D12 textures, views and transient heap classes are native") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::TextureDescription description;
    description.name = "D3D12 target";
    description.format = cy::rhi::Format::Rgba8Unorm;
    description.extent = {64, 64, 1};
    description.usage = cy::rhi::TextureUsage::ColorAttachment | cy::rhi::TextureUsage::Sampled;
    const auto texture = fixture.device().create_texture(description);
    CY_REQUIRE(texture);
    cy::rhi::TextureViewDescription view_description;
    view_description.texture = *texture;
    const auto view = fixture.device().create_texture_view(view_description);
    CY_REQUIRE(view);
    fixture.device().destroy_texture_view(*view);
    fixture.device().destroy_texture(*texture);

    const auto transient_texture = fixture.device().create_transient_texture(description);
    CY_REQUIRE(transient_texture);
    cy::rhi::BufferDescription buffer_description;
    buffer_description.size = 64 * 1024;
    buffer_description.usage = cy::rhi::BufferUsage::Storage;
    const auto transient_buffer = fixture.device().create_transient_buffer(buffer_description);
    CY_REQUIRE(transient_buffer);
    const auto texture_memory = fixture.device().texture_memory_requirements(*transient_texture);
    const auto buffer_memory = fixture.device().buffer_memory_requirements(*transient_buffer);
    CY_REQUIRE(texture_memory);
    CY_REQUIRE(buffer_memory);
    const cy::rhi::MemoryPoolClass common =
        meet(texture_memory->pool_class, buffer_memory->pool_class);
    const auto identity = cy::rhi::d3d12::d3d12_runtime_identity();
    if (identity.resource_heap_tier == 1) {
        CY_CHECK(common.empty());
    } else {
        CY_CHECK_FALSE(common.empty());
        const cy::u64 bytes =
            texture_memory->size > buffer_memory->size ? texture_memory->size : buffer_memory->size;
        CY_REQUIRE(fixture.device().reserve_transient_memory(bytes, common));
        CY_REQUIRE(fixture.device().bind_transient(*transient_texture, 0));
        CY_REQUIRE(fixture.device().bind_transient(*transient_buffer, 0));
    }
    fixture.device().release_transient_resources();
}

CY_TEST_CASE("D3D12 command lists submit under the debug layer") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    CY_REQUIRE(fixture.device().begin_frame());
    const auto command =
        fixture.device().acquire_command_buffer(cy::rhi::QueueKind::Graphics, false);
    CY_REQUIRE(command);
    CY_REQUIRE(fixture.device().begin_command_buffer(*command));
    CY_REQUIRE(fixture.device().end_command_buffer(*command));
    cy::rhi::SubmitInfo submit;
    submit.command_buffers = {&*command, 1};
    CY_REQUIRE(fixture.device().submit(submit));
    CY_REQUIRE(fixture.device().end_frame());
    CY_REQUIRE(fixture.device().wait_idle());
    CY_CHECK_EQ(fixture.device().statistics().validation_errors, 0U);
}
