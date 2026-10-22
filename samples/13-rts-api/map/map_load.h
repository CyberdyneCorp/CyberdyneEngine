// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_MAP_LOAD_H
#define CY_SAMPLE_RTS_MAP_LOAD_H
// map_load.h — the RTS test map, loaded the way a game loads content: a cooked `.cypak` mounted
// into the virtual filesystem, every asset read back BY ID through `cy::assets::AssetSystem`, each
// payload checked against the cooked-asset header's kind and content hash before it is decoded.
//
// Nothing here knows the cook. A package another build wrote, or one a test damaged, is read the
// same way, and refused with the reason when a header, a hash or a payload does not hold.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>

#include <string>

#include "map_format.h"

namespace sample::rts::map {

/// What a load read.
struct LoadReport {
    /// Assets the asset system served.
    u32 assets = 0;
    /// Bytes of cooked payload they held.
    u64 bytes = 0;
    /// Wall time from the mount to the last decode.
    f64 load_ms = 0.0;
    /// `content_digest` of what was decoded.
    u64 digest = 0;
};

/// Mount `package` and read the whole map out of it. `detail` names the asset that failed.
[[nodiscard]] cy::Status load_map(cy::Allocator& allocator, const std::string& package,
                                  MapContent& out, LoadReport& report, std::string& detail);

/// Decode one asset's cooked bytes — header, hash, payload — into `out`, by its id. What `load_map`
/// does for each asset, exposed so a test can hand it damaged bytes.
[[nodiscard]] cy::Status decode_asset(cy::AssetId id, cy::Span<const u8> cooked, MapContent& out);

}  // namespace sample::rts::map

#endif  // CY_SAMPLE_RTS_MAP_LOAD_H
