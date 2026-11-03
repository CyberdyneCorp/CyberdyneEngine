// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_LETTERBOX_H
#define CY_SAMPLE_RTS_API_LETTERBOX_H
// letterbox.h — where a picture of one size lands in a window of another: as large as fits with
// its aspect kept, centred, with bars on the two sides it does not fill. `--play` draws its frame
// into the window this way (present_pass.h) and maps the window's pointer back into the view this
// way (play.cpp), so a click lands on the pixel of the frame it was made over. Header-only and
// free of any device, so `integration.rts_api_play` checks it directly.

#include <cy/core/base/types.h>

#include <algorithm>

namespace sample::rts {

/// A rectangle of the target, in its own units.
struct Letterbox {
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;
    cy::f32 width = 0.0F;
    cy::f32 height = 0.0F;
};

/// Where a `source_width` by `source_height` picture lands in a `target_width` by
/// `target_height` one. Empty when either has no area.
[[nodiscard]] inline Letterbox letterbox(cy::f32 source_width, cy::f32 source_height,
                                         cy::f32 target_width, cy::f32 target_height) noexcept {
    Letterbox box;
    if (source_width <= 0.0F || source_height <= 0.0F || target_width <= 0.0F ||
        target_height <= 0.0F) {
        return box;
    }
    const cy::f32 scale = std::min(target_width / source_width, target_height / source_height);
    box.width = source_width * scale;
    box.height = source_height * scale;
    box.x = (target_width - box.width) * 0.5F;
    box.y = (target_height - box.height) * 0.5F;
    return box;
}

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_LETTERBOX_H
