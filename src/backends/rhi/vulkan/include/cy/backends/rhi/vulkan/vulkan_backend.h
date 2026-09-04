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

}  // namespace cy::rhi::vulkan
