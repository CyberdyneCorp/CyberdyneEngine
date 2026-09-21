// SPDX-License-Identifier: MIT
#pragma once

#include "mobile_vfx_msl.h"

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::ship_ios {

// FNV-1a over mobile_vfx.slang. The app compares this with the source generated from the mobile
// effect before it accepts the checked-in MSL, so editing the graph without recooking cannot ship
// a plausible-looking but stale particle kernel.
inline constexpr u64 kMobileVfxSlangHash = 0x0f75eb8bf45af909ULL;
inline constexpr usize kMobileVfxSlangBytes = 17663;

[[nodiscard]] inline u64 source_hash(Span<const char> source) noexcept {
    u64 value = 14695981039346656037ULL;
    for (const char byte : source) {
        value ^= static_cast<u8>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}

}  // namespace cy::ship_ios
