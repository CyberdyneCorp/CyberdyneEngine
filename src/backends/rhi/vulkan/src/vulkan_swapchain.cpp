// The swapchain, over a surface the platform produced. Task 2.3.2.
//
// THE RHI NEVER TALKS TO A WINDOW SYSTEM. `SwapchainDescription::native_surface` is what
// DisplayServer::create_surface() returned for GraphicsApi::Vulkan — a VkSurfaceKHR the platform
// backend built with SDL's own Vulkan entry points. design.md §4's rule has waited three milestones
// for this, its first consumer: there is no platform #ifdef in this file, no SDL header, and no
// window-system extension called from here.
//
// The surface is NOT owned by the swapchain. DisplayServer::destroy_surface() owns it, because the
// platform created it, and a swapchain that destroyed it would leave the window's own bookkeeping
// pointing at a freed object.

#include "vulkan_device.h"

namespace cy::rhi::vulkan {
namespace {

/// `value`, bounded to [low, high]. std::clamp would do, but it takes a comparator by reference and
/// the three-argument form reads no better here than the name does.
u32 clamp_to(u32 value, u32 low, u32 high) noexcept {
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

}  // namespace

Expected<SwapchainHandle, Error> VulkanDevice::create_swapchain(const SwapchainDescription& desc) {
    if (desc.native_surface == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "a swapchain needs the VkSurfaceKHR DisplayServer::create_surface() produced "
                    "for GraphicsApi::Vulkan");
    }
    if (desc.extent.width == 0 || desc.extent.height == 0) {
        return fail(ErrorCode::InvalidArgument, "swapchain: a zero extent presents nothing");
    }

    // A surface handle is 64 bits on every platform (VK_DEFINE_NON_DISPATCHABLE_HANDLE), and the
    // DisplayServer seam carries it as a void*. The round trip is the one the seam was designed
    // for. NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto* surface = reinterpret_cast<VkSurfaceKHR>(desc.native_surface);

    const u32 graphics_family = queue_families_[static_cast<u32>(QueueKind::Graphics)];
    VkBool32 supported = VK_FALSE;
    CY_VK_TRY(vkGetPhysicalDeviceSurfaceSupportKHR(physical_, graphics_family, surface, &supported),
              "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (supported != VK_TRUE) {
        return fail(ErrorCode::Unsupported,
                    "the graphics queue family cannot present to this surface");
    }

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) — see above.
    VkSurfaceCapabilitiesKHR capabilities{};
    CY_VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface, &capabilities),
              "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    u32 format_count = 0;
    CY_VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface, &format_count, nullptr),
              "vkGetPhysicalDeviceSurfaceFormatsKHR");
    Array<VkSurfaceFormatKHR> formats(*allocator_);
    if (Status sized = formats.resize(format_count); !sized) {
        return make_unexpected(sized.error());
    }
    CY_VK_TRY(
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface, &format_count, formats.data()),
        "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (format_count == 0) {
        return fail(ErrorCode::Unsupported, "the surface reports no formats");
    }

    // What was asked for, if the surface has it; otherwise the surface's first, reported back
    // through SwapchainInfo. A swapchain is a negotiation, and the honest answer is what it got.
    VkSurfaceFormatKHR chosen = formats[0];
    const VkFormat wanted = to_vulkan(desc.preferred_format);
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == wanted) {
            chosen = candidate;
            break;
        }
    }

    u32 mode_count = 0;
    CY_VK_TRY(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface, &mode_count, nullptr),
              "vkGetPhysicalDeviceSurfacePresentModesKHR");
    Array<VkPresentModeKHR> modes(*allocator_);
    if (Status sized = modes.resize(mode_count); !sized) {
        return make_unexpected(sized.error());
    }
    CY_VK_TRY(
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface, &mode_count, modes.data()),
        "vkGetPhysicalDeviceSurfacePresentModesKHR");

    // FIFO is the only mode a Vulkan implementation must support, so it is the fallback and never a
    // failure. A caller that asked for Mailbox and got FIFO finds out by reading SwapchainInfo.
    VkPresentModeKHR present = VK_PRESENT_MODE_FIFO_KHR;
    const VkPresentModeKHR requested = to_vulkan(desc.present_mode);
    for (const VkPresentModeKHR candidate : modes) {
        if (candidate == requested) {
            present = requested;
            break;
        }
    }

    VkExtent2D extent{desc.extent.width, desc.extent.height};
    if (capabilities.currentExtent.width != ~0U) {
        extent = capabilities.currentExtent;
    } else {
        extent.width = clamp_to(extent.width, capabilities.minImageExtent.width,
                                capabilities.maxImageExtent.width);
        extent.height = clamp_to(extent.height, capabilities.minImageExtent.height,
                                 capabilities.maxImageExtent.height);
    }

    u32 image_count = desc.min_image_count < capabilities.minImageCount ? capabilities.minImageCount
                                                                        : desc.min_image_count;
    if (capabilities.maxImageCount != 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }

    // Vulkan's own flag-bit enums have no zero enumerator, and zero-initialising the structure
    // before filling in what matters is the API's documented idiom.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
    VkSwapchainCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface;
    info.minImageCount = image_count;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = present;
    info.clipped = VK_TRUE;

    SwapchainRecord record(*allocator_);
    record.surface = surface;
    record.requested = desc.present_mode;
    CY_VK_TRY(vkCreateSwapchainKHR(device_, &info, nullptr, &record.swapchain),
              "vkCreateSwapchainKHR");

    u32 actual_count = 0;
    CY_VK_TRY(vkGetSwapchainImagesKHR(device_, record.swapchain, &actual_count, nullptr),
              "vkGetSwapchainImagesKHR");
    Array<VkImage> images(*allocator_);
    if (Status sized = images.resize(actual_count); !sized) {
        return make_unexpected(sized.error());
    }
    CY_VK_TRY(vkGetSwapchainImagesKHR(device_, record.swapchain, &actual_count, images.data()),
              "vkGetSwapchainImagesKHR");

    record.info.format = from_vulkan(chosen.format);
    record.info.present_mode = from_vulkan(present);
    record.info.extent = Extent2D{extent.width, extent.height};
    record.info.image_count = actual_count;

    for (u32 index = 0; index < actual_count; ++index) {
        // The images belong to the presentation engine. They are wrapped in the engine's handle
        // model so that the render graph can import one exactly like any other texture, and
        // `owned_by_swapchain` is what stops destroy_texture() freeing something it does not own.
        VulkanTexture texture;
        texture.desc.name = "swapchain image";
        texture.desc.format = record.info.format;
        texture.desc.extent = Extent3D{extent.width, extent.height, 1};
        texture.desc.usage = TextureUsage::ColorAttachment | TextureUsage::TransferDestination |
                             TextureUsage::TransferSource;
        texture.name.assign("swapchain image");
        texture.desc.name = texture.name.text;
        texture.image = images[index];
        texture.owned_by_swapchain = true;

        Expected<TextureHandle, Error> texture_handle = textures_.create(texture);
        if (!texture_handle) {
            return make_unexpected(texture_handle.error());
        }
        if (Status pushed = record.textures.push_back(*texture_handle); !pushed) {
            return make_unexpected(pushed.error());
        }

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = images[index];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = chosen.format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;

        VulkanTextureView view;
        view.texture = *texture_handle;
        view.range = SubresourceRange{0, 1, 0, 1};
        CY_VK_TRY(vkCreateImageView(device_, &view_info, nullptr, &view.view),
                  "vkCreateImageView (swapchain)");
        Expected<TextureViewHandle, Error> view_handle = views_.create(view);
        if (!view_handle) {
            vkDestroyImageView(device_, view.view, nullptr);
            return make_unexpected(view_handle.error());
        }
        if (Status pushed = record.views.push_back(*view_handle); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    name_object(reinterpret_cast<u64>(record.swapchain), VK_OBJECT_TYPE_SWAPCHAIN_KHR, desc.name);
    return swapchains_.create(std::move(record));
}

void VulkanDevice::destroy_swapchain(SwapchainHandle handle) noexcept {
    SwapchainRecord* record = swapchains_.resolve(handle);
    if (record == nullptr) {
        return;
    }
    (void)vkDeviceWaitIdle(device_);
    for (const TextureViewHandle view : record->views) {
        if (VulkanTextureView* stored = views_.resolve(view); stored != nullptr) {
            vkDestroyImageView(device_, stored->view, nullptr);
            (void)views_.destroy(view);
        }
    }
    for (const TextureHandle texture : record->textures) {
        // The VkImage belongs to the presentation engine; only the handle is ours to release.
        (void)textures_.destroy(texture);
    }
    vkDestroySwapchainKHR(device_, record->swapchain, nullptr);
    // The VkSurfaceKHR is NOT destroyed here: DisplayServer created it and DisplayServer owns it.
    (void)swapchains_.destroy(handle);
}

Status VulkanDevice::resize_swapchain(SwapchainHandle handle, Extent2D extent) {
    SwapchainRecord* record = swapchains_.resolve(handle);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "resize_swapchain(): stale handle");
    }
    if (extent.width == 0 || extent.height == 0) {
        return fail(ErrorCode::InvalidArgument, "resize_swapchain(): a zero extent");
    }

    SwapchainDescription description;
    description.name = "swapchain";
    description.native_surface = record->surface;
    description.extent = extent;
    description.preferred_format = record->info.format;
    description.present_mode = record->requested;
    description.min_image_count = record->info.image_count;

    // Recreate rather than patch: a swapchain's images, views and extent are one object as far as
    // the driver is concerned, and rebuilding is the only correct response to an out-of-date one.
    destroy_swapchain(handle);
    Expected<SwapchainHandle, Error> rebuilt = create_swapchain(description);
    if (!rebuilt) {
        return make_unexpected(rebuilt.error());
    }
    return ok();
}

SwapchainInfo VulkanDevice::swapchain_info(SwapchainHandle handle) const noexcept {
    const SwapchainRecord* record = swapchains_.resolve(handle);
    return record != nullptr ? record->info : SwapchainInfo{};
}

Expected<u32, Error> VulkanDevice::acquire_next_image(SwapchainHandle handle,
                                                      SemaphoreHandle signal, u64 timeout_ns) {
    SwapchainRecord* record = swapchains_.resolve(handle);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "acquire_next_image(): stale swapchain handle");
    }
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (!signal.is_null()) {
        const VulkanSemaphore* stored = semaphores_.resolve(signal);
        if (stored == nullptr) {
            return fail(ErrorCode::NotFound, "acquire_next_image(): stale semaphore handle");
        }
        semaphore = stored->semaphore;
    }
    u32 index = 0;
    const VkResult result =
        vkAcquireNextImageKHR(device_, record->swapchain, timeout_ns == 0 ? ~0ULL : timeout_ns,
                              semaphore, VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Answered by resizing, not by failing the frame. Reported as Unavailable so that a caller
        // that does not care about the distinction still handles it as a recoverable condition.
        return fail(ErrorCode::Unavailable, "the swapchain is out of date; resize it");
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return make_unexpected(error_from(result, "vkAcquireNextImageKHR"));
    }
    return index;
}

TextureHandle VulkanDevice::swapchain_texture(SwapchainHandle handle, u32 index) const noexcept {
    const SwapchainRecord* record = swapchains_.resolve(handle);
    if (record == nullptr || index >= record->textures.size()) {
        return TextureHandle{};
    }
    return record->textures[index];
}

TextureViewHandle VulkanDevice::swapchain_view(SwapchainHandle handle, u32 index) const noexcept {
    const SwapchainRecord* record = swapchains_.resolve(handle);
    if (record == nullptr || index >= record->views.size()) {
        return TextureViewHandle{};
    }
    return record->views[index];
}

Status VulkanDevice::present(SwapchainHandle handle, u32 image_index, SemaphoreHandle wait) {
    SwapchainRecord* record = swapchains_.resolve(handle);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "present(): stale swapchain handle");
    }
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (!wait.is_null()) {
        const VulkanSemaphore* stored = semaphores_.resolve(wait);
        if (stored == nullptr) {
            return fail(ErrorCode::NotFound, "present(): stale semaphore handle");
        }
        semaphore = stored->semaphore;
    }

    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = semaphore != VK_NULL_HANDLE ? 1U : 0U;
    info.pWaitSemaphores = semaphore != VK_NULL_HANDLE ? &semaphore : nullptr;
    info.swapchainCount = 1;
    info.pSwapchains = &record->swapchain;
    info.pImageIndices = &image_index;

    const VkResult result =
        vkQueuePresentKHR(queues_[static_cast<u32>(QueueKind::Graphics)], &info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        return fail(ErrorCode::Unavailable, "the swapchain is out of date; resize it");
    }
    if (result != VK_SUCCESS) {
        return make_unexpected(error_from(result, "vkQueuePresentKHR"));
    }
    return ok();
}

}  // namespace cy::rhi::vulkan
