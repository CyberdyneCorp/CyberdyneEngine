#pragma once
// SPDX-License-Identifier: MIT
// Where this program's vertex, colour and index streams live. #77.
//
// ================================================================================================
// WHY IT MATTERS, AND HOW MUCH
// ================================================================================================
//
// The world is drawn three times a frame with water shading on: the opaque frame, and the water's
// refraction and reflection pictures (water_surface.h). The streams those draws read are about
// sixteen megabytes: the terrain's positions and colours, and eleven megabytes of plant proxies,
// sky, stars and sea written on the processor every frame. In `Upload` memory — which on a discrete
// GPU is SYSTEM memory — every one of those draws pulls its streams across the bus, so each redraw
// cost what the first one did. Measured on the RTX 5060 across the 64-frame take: 4 to 5 ms of
// device time a frame, 2.0 to 2.5 ms of it the two water pictures; with the streams in device-local
// memory the processor maps, 1.3 ms and 0.56 ms, and every frame byte-identical. The processor pays
// about 0.7 ms more to write the dynamic streams across the bus once, against about 2.7 ms the
// device no longer spends reading them across it.
//
// ================================================================================================
// WHY A CAPABILITY AND NOT THE MEMORY USE ALONE
// ================================================================================================
//
// `MemoryUse::HostVisibleDeviceLocal` is a request, and not every backend answers it with memory
// the processor can map: D3D12 places it in a default heap. Every stream here is written through a
// mapping, so the request is made only where the device says it can be met, and `Upload` — what
// this program always used — everywhere else. On a unified-memory device the two are the same
// memory and nothing changes.

#include <cy/backends/rhi/capabilities.h>
#include <cy/backends/rhi/resources.h>

namespace cy::sample::world {

/// The memory the world's streams are created in on a device with these capabilities.
[[nodiscard]] inline rhi::MemoryUse geometry_memory(
    const rhi::DeviceCapabilities& capabilities) noexcept {
    return capabilities.has(rhi::Capability::HostVisibleDeviceLocalMemory)
               ? rhi::MemoryUse::HostVisibleDeviceLocal
               : rhi::MemoryUse::Upload;
}

}  // namespace cy::sample::world
