// SPDX-License-Identifier: MIT
#pragma once
// Depth of field: the settings, the constants every dispatch reads, and the host twins of the
// shaders' arithmetic.
//
// `rendering-post-processing` — "Depth of field": "computed from a physically parameterised circle
// of confusion derived from focal length, aperture, focus distance, and sensor size, with an
// artistic override", gathered "with separate near and far fields, a configurable bokeh shape …
// and correct occlusion handling so near-field blur bleeds over in-focus geometry".
//
// ================================================================================================
// THE CIRCLE OF CONFUSION IS `circle_of_confusion`, IN PIXELS
// ================================================================================================
//
// `post/effects.h`'s thin-lens `circle_of_confusion` is the blur circle's DIAMETER on the sensor
// as a fraction of the sensor height, signed: negative in front of the focus plane. The image is
// the sensor, so the blur RADIUS in pixels is half that fraction times the image height:
//
//     r(d) = K (d - F) / d,     K = H/2 * A f / ((F - f) s) * artistic_scale,   A = f / N
//
// with d the distance along the optical axis, F the focus distance, f the focal length, N the
// f-number, s the sensor height and H the image height in pixels. `make_dof_constants` folds that
// into K and F, and `coc_radius_pixels` below is the device's expression, which `unit.render_dof`
// holds to `circle_of_confusion` at every distance it samples. N = infinity — a pinhole — is
// K = 0: no pixel has a circle to blur by, and the stage is the identity (render.depth_of_field
// asserts it byte for byte). The sky, cleared to depth 0, is at infinity: r = K.
//
// The distance is the one the depth buffer encodes. `DofView::projection` is the matrix the depth
// was written with, and `view_distance` inverts it for any perspective or orthographic matrix:
// d = (m23 - z m33) / (m22 - z m32).
//
// ================================================================================================
// THE FREE PARAMETERS, AND WHY EACH EXISTS
// ================================================================================================
//
// None of the blur's SIZE is free: it is the lens's. What is free is what a finite gather costs:
//
//   `max_radius_fraction`   the largest radius gathered, as a fraction of the image height. A
//                           circle beyond it is clamped. It bounds the gather's taps and sizes
//                           the near field's tiles, which must be at least that wide.
//   `max_rings`             the gather's sampling density: rings of 6k taps at spacing
//                           R / (rings + 1/2), 1 + 3k(k + 1) taps. Eight by default: a tap a
//                           texel out to eight texels of radius. Not a physical quantity.
//   `blades`                the aperture's shape. Zero is a circle; five or more straight blades
//                           make that polygon, inscribed in the circle, the shape a defocused
//                           point takes — its area is what the near field's coverage divides by.
//
// And one band stated rather than tuned: a circle of radius below ONE PIXEL is in focus. It is the
// target's own resolution limit; the composite blends from the sharp pixel to the blurred layers
// between one and two pixels of radius, where the half-resolution layers first resolve a disc.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/rendering/post/effects.h>

namespace cy::rendering::depth_of_field {

/// Radii below this, in pixels of the target, are in focus: the resolution limit.
inline constexpr f32 kFocusBandPixels = 1.0F;
/// The most rings a gather may take.
inline constexpr u32 kMaxRings = 16;
/// The most aperture blades. A polygon of more is indistinguishable from the circle at any radius
/// this gather reaches.
inline constexpr u32 kMaxBlades = 16;

struct DofSettings {
    /// Focal length, f-number, focus distance, sensor height and the artistic override, as
    /// `post/effects.h` defines them. `lens.aperture` = infinity is a pinhole. `autofocus_speed` is
    /// the caller's, through `track_focus`; nothing here reads it.
    DepthOfFieldSettings lens{};
    /// 0 for a circular aperture, else the number of straight blades, 5 to `kMaxBlades`.
    u32 blades = 0;
    /// The polygon's rotation, radians, counter-clockwise from +x in the image.
    f32 blade_rotation = 0.0F;
    /// The largest blur radius gathered, as a fraction of the image height. See the header.
    f32 max_radius_fraction = 0.02F;
    /// The gather's rings. See the header.
    u32 max_rings = 8;
};

/// What one frame's view contributes.
struct DofView {
    /// The projection the depth was written with: reversed-Z, perspective or orthographic.
    Mat4 projection = Mat4::identity();
    u32 width = 0;
    u32 height = 0;
};

/// `dof_common.slang`'s `CyDofConstants`, word for word.
struct alignas(16) DofConstants {
    /// The projection's m22, m23, m32 and m33 (row, column): the depth's inverse.
    f32 depth[4] = {};
    /// x: K, the radius in pixels at infinity. y: F, the focus distance in metres. z: the largest
    /// radius gathered, in pixels. w: the focus band, in pixels.
    f32 lens[4] = {};
    /// x: blades (0 for a circle). y: their rotation. z: the aperture's area over its circumradius
    /// squared — pi for the circle. w: the most rings a gather takes.
    f32 aperture[4] = {};
    /// The target's width and height, then the half-resolution layers'.
    u32 extent[4] = {};
    /// x: the tile edge in half-resolution texels. yz: the tile grid. w: unused.
    u32 tiles[4] = {};
};

static_assert(sizeof(DofConstants) == 80, "dof_common.slang's constants are 80 bytes");

/// The half-resolution layers' extent: every full-resolution texel lands in one.
[[nodiscard]] constexpr u32 half_extent(u32 full) noexcept {
    return (full + 1U) / 2U;
}

/// The focal length a lens needs for a vertical field of view on a sensor of this height, in
/// millimetres — so the lens the circle of confusion is computed for is the lens the projection
/// draws with.
[[nodiscard]] f32 focal_length_for_field_of_view(f32 vertical_fov_radians,
                                                 f32 sensor_height_mm) noexcept;

/// The dispatches' constants. Refuses an empty view, an out-of-range blade or ring count, a
/// maximum radius outside (0, 0.1] of the image, and a focus distance that is not in front of the
/// lens.
[[nodiscard]] Expected<DofConstants, Error> make_dof_constants(const DofSettings& settings,
                                                               const DofView& view) noexcept;

/// The distance along the optical axis a depth-buffer value encodes. Infinity for the cleared sky.
[[nodiscard]] f32 view_distance(const DofConstants& constants, f32 depth) noexcept;

/// The signed blur radius in pixels at a distance, unclamped: `dof_common.slang`'s `cyDofRadius`.
[[nodiscard]] f32 coc_radius_pixels(const DofConstants& constants, f32 distance) noexcept;

/// The same, clamped to the largest radius gathered, from a depth-buffer value.
[[nodiscard]] f32 coc_radius_at_depth(const DofConstants& constants, f32 depth) noexcept;

/// How far the aperture reaches in a direction, as a fraction of its circumradius: 1 for the
/// circle, the inscribed polygon's radial distance otherwise. `cyDofApertureExtent`.
[[nodiscard]] f32 aperture_extent(const DofConstants& constants, f32 angle_radians) noexcept;

/// The area of an aperture of unit circumradius: pi, or the inscribed polygon's (n/2) sin(2 pi/n).
[[nodiscard]] f32 aperture_area(u32 blades) noexcept;

/// The gather's taps for a radius: rings `ceil(radius)` up to `max_rings`, 1 + 3k(k+1) taps.
[[nodiscard]] u32 gather_rings(f32 radius, u32 max_rings) noexcept;
[[nodiscard]] constexpr u32 gather_taps(u32 rings) noexcept {
    return 1U + (3U * rings * (rings + 1U));
}

/// The radius a gather of `rings` rings spans for a blur of `radius` texels: the outermost ring on
/// the outer edge of the reach's one-texel ramp, half a texel beyond the disc, so the aperture's
/// rim is sampled rather than cut off by the rings. `cyDofSpan`.
[[nodiscard]] f32 gather_span(f32 radius, u32 rings) noexcept;

/// One tap of a gather of `rings` rings over radius `radius`: its offset and the area of the disc
/// it stands for. Tap 0 is the centre; ring k holds 6k taps at radius k R / (rings + 1/2). The
/// areas sum to pi R^2. `cyDofTap`.
struct GatherTap {
    Vec2 offset{0.0F, 0.0F};
    f32 area = 0.0F;
};
[[nodiscard]] GatherTap gather_tap(u32 index, u32 rings, f32 radius) noexcept;

}  // namespace cy::rendering::depth_of_field
