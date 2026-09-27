// SPDX-License-Identifier: MIT
#pragma once
// How long a streak is, measured off a picture rather than read off the constants that made it.
// Shared by the host suite and the device suite, so the two measure one thing one way.
//
// A uniform object whose edge crosses a pixel for a fraction of the shutter covers that pixel for
// that fraction: across the edge the blurred luminance RAMPS from one plateau to the other, and the
// ramp is as long as the distance the edge travelled while the shutter was open. Its 10 %-to-90 %
// rise is 80 % of that length, which is what `edge_ramp_pixels` measures — the two plateaus are
// read `reach` pixels either side of where the edge is in the UNBLURRED frame, so the measurement
// needs nothing from the filter but its output.

#include <cy/core/base/types.h>

#include <cmath>
#include <vector>

namespace cy::motion_test {

/// Rec. 709 luminance of a linear colour.
[[nodiscard]] inline f32 luminance(f32 red, f32 green, f32 blue) noexcept {
    return (0.2126F * red) + (0.7152F * green) + (0.0722F * blue);
}

/// Where along `row` the normalised profile first crosses `level`, between `from` and `to`,
/// interpolated linearly between the two texels that straddle it. -1 when it never does.
[[nodiscard]] inline f32 crossing(const std::vector<f32>& profile, u32 from, u32 to,
                                  f32 level) noexcept {
    for (u32 x = from; x < to; ++x) {
        const f32 a = profile[x];
        const f32 b = profile[x + 1U];
        if ((a - level) * (b - level) <= 0.0F && a != b) {
            return static_cast<f32>(x) + ((level - a) / (b - a));
        }
    }
    return -1.0F;
}

/// The length of the ramp across an edge at `edge`, in pixels: the 10 %-to-90 % rise of the
/// profile between the plateaus at `edge - reach` and `edge + reach`, divided by 0.8. Zero for a
/// profile with no contrast, and negative when the rise could not be found.
[[nodiscard]] inline f32 edge_ramp_pixels(const std::vector<f32>& row, u32 edge,
                                          u32 reach) noexcept {
    if (edge < reach || edge + reach >= row.size()) {
        return -1.0F;
    }
    const f32 low = row[edge - reach];
    const f32 high = row[edge + reach];
    if (std::fabs(high - low) < 1e-6F) {
        return 0.0F;
    }
    std::vector<f32> profile(row.size(), 0.0F);
    for (u32 x = edge - reach; x <= edge + reach; ++x) {
        profile[x] = (row[x] - low) / (high - low);
    }
    const f32 ten = crossing(profile, edge - reach, edge + reach, 0.1F);
    const f32 ninety = crossing(profile, edge - reach, edge + reach, 0.9F);
    if (ten < 0.0F || ninety < 0.0F) {
        return -1.0F;
    }
    return std::fabs(ninety - ten) / 0.8F;
}

}  // namespace cy::motion_test
