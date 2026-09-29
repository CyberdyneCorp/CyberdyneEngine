// SPDX-License-Identifier: MIT
#pragma once

#include <cy/core/memory/array.h>

namespace cy::vfx {

/// Encode the node definitions registered by the VFX compiler for editor-service consumers.
/// Schema 2 uses the shared graph-catalogue envelope and includes typed property descriptors.
[[nodiscard]] Status encode_vfx_catalogue(Array<u8>& out) noexcept;

}  // namespace cy::vfx
