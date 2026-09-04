#pragma once
// The one place Vulkan is included, and the small helpers every file here needs. Task 2.3.1.
//
// VK_NO_PROTOTYPES plus volk: entry points are fetched per instance and per device rather than
// through the loader's dispatch trampoline, so the engine links no Vulkan library at all. A machine
// with no driver still builds and still links this backend; it fails at volkInitialize(), which is
// what `BackendRegistration::is_available` reports before anything tries to create a device.
//
// NO VULKAN TYPE LEAVES THIS DIRECTORY. tools/layercheck/layercheck.py's `gpuapi` check fails the
// build if a Vulkan header is included above src/backends/, and the RHI's own vocabulary
// (cy/backends/rhi/types.h) is engine-owned precisely so that the render graph one layer up needs
// no graphics SDK to compile.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/assert.h>

#define VK_NO_PROTOTYPES
#include <volk.h>

namespace cy::rhi::vulkan {

/// The API version the instance asks for.
///
/// 1.3, and not more. Everything M3 needs — synchronization2, dynamic rendering and timeline
/// semaphores — is core 1.2 or 1.3, and `rhi-and-render-graph` states 1.3 as the minimum. On this
/// project's development machine the loader reports 1.3.275 while the NVIDIA ICD reports 1.4.312;
/// asking for 1.4 there would fail against the loader for features nothing uses. (M3 spike, 6g.)
inline constexpr u32 kInstanceApiVersion = VK_API_VERSION_1_3;

/// Turn a VkResult into a cy::Error. The message is the enumerator's own spelling, because a
/// diagnostic that says "Vulkan failed" sends the reader to look the number up.
[[nodiscard]] const char* result_name(VkResult result) noexcept;
[[nodiscard]] Error error_from(VkResult result, const char* what) noexcept;

/// `if (VkResult r = vkDoThing(); r != VK_SUCCESS) return make_unexpected(error_from(r, "..."));`
/// spelled once. Used only where the caller returns an Expected or a Status.
#define CY_VK_TRY(expression, what)                                                             \
    do {                                                                                        \
        const VkResult cy_vk_result_ = (expression);                                            \
        if (cy_vk_result_ != VK_SUCCESS) {                                                      \
            return ::cy::make_unexpected(::cy::rhi::vulkan::error_from(cy_vk_result_, (what))); \
        }                                                                                       \
    } while (false)

}  // namespace cy::rhi::vulkan
