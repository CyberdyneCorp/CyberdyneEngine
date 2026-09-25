// SPDX-License-Identifier: MIT
#pragma once

#include <cy/vfx/asset.h>

#include <string_view>

namespace cy::vfx {

/// Read an editor .cyvfxdoc draft into the engine asset model. Both payload versions are accepted;
/// graph validation and compilation remain the compiler's responsibility.
[[nodiscard]] Expected<VfxSystemAsset, Error> read_authoring_document(
    std::string_view source, Allocator& allocator) noexcept;

}  // namespace cy::vfx
