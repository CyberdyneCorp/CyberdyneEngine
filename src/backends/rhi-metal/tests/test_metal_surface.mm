// SPDX-License-Identifier: MIT
#include <cy/test/test.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cy/backends/rhi-metal/backend.h>
#include <cy/backends/rhi/backend.h>
#include <cy/core/memory/domain.h>
#include <cy/core/memory/system_allocator.h>

namespace {

class Fixture {
public:
    Fixture() noexcept : allocator_(cy::system_allocator(cy::MemoryDomain::Gpu)) {
        (void)cy::rhi::metal::register_metal_backend();
        cy::rhi::DeviceDescription description;
        description.application_name = "integration.rhi_metal_surface";
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

CY_TEST_CASE("Metal consumes a CAMetalLayer supplied by the display seam") {
    Fixture fixture;
    CY_REQUIRE(fixture.ok());
    cy::rhi::Device& device = fixture.device();

    CAMetalLayer* layer = [CAMetalLayer layer];
    cy::rhi::SwapchainDescription description;
    description.name = "headless Metal surface";
    description.native_surface = (__bridge void*)layer;
    description.extent = {8, 8};
    description.preferred_format = cy::rhi::Format::Bgra8Unorm;
    description.present_mode = cy::rhi::PresentMode::Immediate;
    description.min_image_count = 3;
    const auto swapchain = device.create_swapchain(description);
    CY_REQUIRE(swapchain);

    const cy::rhi::SwapchainInfo info = device.swapchain_info(*swapchain);
    CY_CHECK_EQ(info.format, cy::rhi::Format::Bgra8Unorm);
    CY_CHECK_EQ(info.extent.width, 8U);
    CY_CHECK_EQ(info.extent.height, 8U);
    CY_CHECK_EQ(info.image_count, 1U);
    CY_CHECK((__bridge void*)layer.device == device.native_handle());
    CY_CHECK_EQ(layer.maximumDrawableCount, 3U);

    const auto acquired = device.create_semaphore();
    const auto rendered = device.create_semaphore();
    CY_REQUIRE(acquired);
    CY_REQUIRE(rendered);
    const auto image_index = device.acquire_next_image(*swapchain, *acquired, 5'000'000'000ULL);
    CY_REQUIRE(image_index);
    CY_CHECK_EQ(*image_index, 0U);
    const cy::rhi::TextureHandle texture = device.swapchain_texture(*swapchain, *image_index);
    const cy::rhi::TextureViewHandle view = device.swapchain_view(*swapchain, *image_index);
    CY_REQUIRE_FALSE(texture.is_null());
    CY_REQUIRE_FALSE(view.is_null());

    cy::rhi::BufferDescription readback_description;
    readback_description.name = "surface pixel readback";
    readback_description.size = 8 * 8 * 4;
    readback_description.usage = cy::rhi::BufferUsage::TransferDestination;
    readback_description.memory = cy::rhi::MemoryUse::Readback;
    const auto readback = device.create_buffer(readback_description);
    CY_REQUIRE(readback);

    const auto command = device.acquire_command_buffer(cy::rhi::QueueKind::Graphics, false);
    CY_REQUIRE(command);
    CY_REQUIRE(device.begin_command_buffer(*command));
    cy::rhi::CommandBuffer* commands = device.command_buffer(*command);
    CY_REQUIRE(commands != nullptr);
    cy::rhi::RenderAttachment attachment;
    attachment.view = view;
    attachment.load = cy::rhi::LoadOp::Clear;
    attachment.store = cy::rhi::StoreOp::Store;
    attachment.clear.color[0] = 0.0F;
    attachment.clear.color[1] = 0.0F;
    attachment.clear.color[2] = 1.0F;
    attachment.clear.color[3] = 1.0F;
    cy::rhi::RenderingInfo rendering;
    rendering.render_area = {0, 0, 8, 8};
    rendering.color_attachments = {&attachment, 1};
    commands->begin_rendering(rendering);
    commands->end_rendering();
    cy::rhi::BufferTextureCopy copy;
    copy.texture_extent = {8, 8, 1};
    commands->copy_texture_to_buffer(texture, *readback, {&copy, 1});
    CY_REQUIRE(device.end_command_buffer(*command));

    cy::rhi::SubmitInfo submit;
    submit.command_buffers = {&*command, 1};
    submit.wait_binary = *acquired;
    submit.signal_binary = *rendered;
    const auto signal = device.submit(submit);
    CY_REQUIRE(signal);
    CY_REQUIRE(device.present(*swapchain, *image_index, *rendered));
    CY_REQUIRE(device.wait_timeline(cy::rhi::QueueKind::Graphics, *signal,
                                    5'000'000'000ULL));
    CY_REQUIRE(device.wait_idle());

    const auto* pixel = static_cast<const cy::u8*>(device.buffer_mapped_pointer(*readback));
    CY_REQUIRE(pixel != nullptr);
    CY_CHECK_EQ(pixel[0], 0xFFU);
    CY_CHECK_EQ(pixel[1], 0x00U);
    CY_CHECK_EQ(pixel[2], 0x00U);
    CY_CHECK_EQ(pixel[3], 0xFFU);

    CY_REQUIRE(device.resize_swapchain(*swapchain, {16, 12}));
    CY_CHECK_EQ(device.swapchain_info(*swapchain).extent.width, 16U);
    CY_CHECK_EQ(device.swapchain_info(*swapchain).extent.height, 12U);

    device.destroy_buffer(*readback);
    device.destroy_semaphore(*rendered);
    device.destroy_semaphore(*acquired);
    device.destroy_swapchain(*swapchain);
}
