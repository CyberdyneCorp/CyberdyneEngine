#pragma once
// The Vulkan backend's whole public surface. Task 2.3.1.
//
// One function. Everything else about this backend is reached through cy::rhi::Device, which is the
// point: `rhi-and-render-graph` requires the renderer to branch on capabilities and never on
// backend identity, so a backend that exposed its own interface would be a backend the renderer
// could accidentally depend on.
//
// NO VULKAN TYPE APPEARS IN THIS HEADER, and none can: it is included from above src/backends/,
// where tools/layercheck/layercheck.py's `gpuapi` check fails the build on a Vulkan include.
//
// The module is excluded from the link entirely when CY_RENDERER_VULKAN is off, so a dedicated
// server build contains no Vulkan code rather than Vulkan code that is never called.

#include <cy/backends/rhi/backend.h>

namespace cy::rhi::vulkan {

/// Register the Vulkan backend under the name "vulkan".
///
/// Also called by a static initialiser in this module, so linking cy::rhi-vulkan is enough. Exposed
/// so that a host can make the registration a statement rather than a link-order property, which is
/// the kind of thing that works everywhere except the one platform nobody tested. Idempotent.
///
/// Registration does not create a device and does not require a driver: the registration's
/// availability probe answers whether a loader is present, so `cy::rhi::create_device()` falls back
/// to the null backend on a machine with no GPU rather than failing.
Status register_vulkan_backend() noexcept;

/// The VkInstance this backend's devices were created against, as an opaque handle.
///
/// M11.d task 8.2, and it exists because presenting to a window needs it. A Vulkan surface is
/// created by the PLATFORM — `DisplayServer::create_surface()` — and a `VkSurfaceKHR` is created
/// against a `VkInstance`, so `SurfaceDescription::api_instance` has to be given one. Nothing in
/// `cy::rhi::Device` exposes it: `native_handle()` is the VkDevice, which is the wrong object.
/// Until M11.d nothing had ever built a swapchain on a window, so nothing had noticed.
///
/// It is on the BACKEND rather than on `Device` deliberately. A method on `cy::rhi::Device` would
/// be an instance handle every backend has to answer and the renderer could branch on, which is
/// exactly what `rhi-and-render-graph`'s "branch on capabilities, never on backend identity"
/// refuses. Reaching it requires naming `cy::rhi::vulkan`, and only a host that already knows it
/// selected Vulkan can do that.
///
/// PRECONDITION: `device` came from this backend — `device.capabilities().backend() ==
/// BackendKind::Vulkan`. The engine compiles without RTTI, so this cannot be checked here; a
/// caller that passes a device from another backend has undefined behaviour, which is the same
/// contract `Device::native_handle()` carries and documents.
///
/// Still a `void*` rather than a `VkInstance`: no Vulkan type may appear in this header, and
/// `tools/layercheck/layercheck.py`'s `gpuapi` check fails the build on one.
[[nodiscard]] void* vulkan_instance_handle(Device& device) noexcept;

}  // namespace cy::rhi::vulkan
