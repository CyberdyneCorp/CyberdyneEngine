// The instance half of the surface seam. M11.d task 8.2.
//
// `vulkan_swapchain.cpp` is the surface's CONSUMER and has been since M3: it is handed a
// VkSurfaceKHR and says, in its own first paragraph, that the RHI never talks to a window system.
// What nobody had needed until this rung is the other end — a VkSurfaceKHR is created against a
// VkInstance, `DisplayServer::create_surface()` is the only thing allowed to create one, and it
// therefore has to be given the instance. Three milestones of rendering never noticed because
// nothing in this repository had ever built a swapchain on a window; every frame before
// samples/11-ship was drawn into an offscreen image and read back.
//
// One function, one cast, no state. It is a separate translation unit rather than three lines in
// vulkan_instance.cpp so that the seam is findable by the name of the problem it solves.

#include <cy/backends/rhi/vulkan/vulkan_backend.h>

#include "vulkan_device.h"

namespace cy::rhi::vulkan {

void* vulkan_instance_handle(Device& device) noexcept {
    // The precondition the header states: the caller has already checked
    // `capabilities().backend() == BackendKind::Vulkan`. There is no RTTI in this engine to check
    // it with here, and a `backend_kind()` on Device would be the identity query the renderer is
    // forbidden to branch on.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto& vulkan_device = static_cast<VulkanDevice&>(device);
    // A VkInstance is a dispatchable handle — a pointer — so this round trip is exact. The
    // DisplayServer seam carries it as a void* for the same reason it carries a surface as one.
    return static_cast<void*>(vulkan_device.vk_instance());
}

}  // namespace cy::rhi::vulkan
