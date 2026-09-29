// SPDX-License-Identifier: MIT
#pragma once

#include <cy/core/memory/array.h>
#include <cy/vfx/runtime.h>

namespace cy::vfx {

/// Publish renderer, simulation-target, and registered interface availability for editor authoring.
/// The device is null until an editor preview device has been attached.
[[nodiscard]] Status encode_authoring_capabilities(Array<u8>& out,
                                                   const DeviceCapability* device) noexcept;

}  // namespace cy::vfx
