// SPDX-License-Identifier: MIT
// Depth of field on the host: the constants the five dispatches read, against the lens and the
// projection they are derived from.
//
// Every function here is the twin of a `dof_common.slang` function of the same name. What these
// cases hold is that the twins say what `post/effects.h`'s thin-lens `circle_of_confusion` and the
// projection matrix say — so that render.depth_of_field, which measures what the device did, is
// measuring the lens and not a second, drifting copy of it.

#include <cy/core/math/projection.h>
#include <cy/core/math/scalar.h>
#include <cy/rendering/depth_of_field/focus.h>
#include <cy/test/test.h>

#include <cmath>
#include <limits>

using namespace cy;
using namespace cy::rendering;
using namespace cy::rendering::depth_of_field;

namespace {

constexpr u32 kWidth = 1920;
constexpr u32 kHeight = 1080;
constexpr f32 kFov = 0.8F;

DofSettings lens_settings() noexcept {
    DofSettings settings;
    settings.lens.sensor_height_mm = 24.0F;
    settings.lens.focal_length_mm = focal_length_for_field_of_view(kFov, 24.0F);
    settings.lens.aperture = 2.0F;
    settings.lens.focus_distance = 6.0F;
    settings.max_radius_fraction = 0.05F;
    return settings;
}

DofView perspective_view() noexcept {
    DofView view;
    view.projection = perspective_reversed_z_infinite(
        kFov, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1F);
    view.width = kWidth;
    view.height = kHeight;
    return view;
}

/// The depth-buffer value a point `distance` in front of the camera is written with.
f32 depth_of(const Mat4& projection, f32 distance) noexcept {
    const f32 z = -distance;
    const f32 clip_z = (projection.at(2, 2) * z) + projection.at(2, 3);
    const f32 clip_w = (projection.at(3, 2) * z) + projection.at(3, 3);
    return clip_z / clip_w;
}

}  // namespace

CY_TEST_CASE("the radius in pixels is half the thin-lens circle of confusion times the height") {
    const DofSettings settings = lens_settings();
    const Expected<DofConstants, Error> constants =
        make_dof_constants(settings, perspective_view());
    CY_REQUIRE(constants.has_value());
    // `circle_of_confusion` is a diameter as a fraction of the sensor height, and the image is
    // the sensor: half of it, in pixels of the image height, is the radius the gather uses.
    for (const f32 distance : {0.3F, 1.0F, 2.5F, 5.0F, 6.0F, 7.5F, 12.0F, 40.0F, 400.0F}) {
        const f32 expected =
            0.5F * circle_of_confusion(settings.lens, distance) * static_cast<f32>(kHeight);
        CY_CHECK_NEAR(coc_radius_pixels(*constants, distance), expected,
                      (1.0e-4F * std::fabs(expected)) + 1.0e-5F);
    }
    // Negative in front of the focus plane, zero on it, positive behind it, and the sky — at
    // infinity — is the limit K.
    CY_CHECK_LT(coc_radius_pixels(*constants, 2.0F), 0.0F);
    CY_CHECK_NEAR(coc_radius_pixels(*constants, 6.0F), 0.0F, 1.0e-6F);
    CY_CHECK_GT(coc_radius_pixels(*constants, 20.0F), 0.0F);
    CY_CHECK_EQ(coc_radius_pixels(*constants, math::kInfinity), constants->lens[0]);
    CY_CHECK_EQ(coc_radius_at_depth(*constants, 0.0F),
                math::min(constants->lens[0], constants->lens[2]));
    // Clamped to the largest radius gathered, which is the stated fraction of the height.
    CY_CHECK_NEAR(constants->lens[2], 0.05F * static_cast<f32>(kHeight), 1.0e-4F);
    CY_CHECK_EQ(coc_radius_at_depth(*constants, depth_of(perspective_view().projection, 0.12F)),
                -constants->lens[2]);
}

CY_TEST_CASE("the focal length is the one the projection's field of view implies") {
    // A 50 mm lens on a 24 mm sensor subtends 26.99 degrees, and back.
    const f32 fov = 2.0F * std::atan(12.0F / 50.0F);
    CY_CHECK_NEAR(focal_length_for_field_of_view(fov, 24.0F), 50.0F, 1.0e-3F);
    CY_CHECK_NEAR(fov * math::kRadToDeg, 26.99F, 0.01F);
}

CY_TEST_CASE("the depth's inverse is exact for the infinite, finite and orthographic matrices") {
    const DofSettings settings = lens_settings();
    const Mat4 matrices[] = {
        perspective_reversed_z_infinite(kFov, 16.0F / 9.0F, 0.1F),
        perspective_reversed_z(kFov, 16.0F / 9.0F, 0.1F, 500.0F),
        orthographic_reversed_z(-8.0F, 8.0F, -4.5F, 4.5F, 0.1F, 500.0F),
    };
    for (const Mat4& projection : matrices) {
        DofView view = perspective_view();
        view.projection = projection;
        const Expected<DofConstants, Error> constants = make_dof_constants(settings, view);
        CY_REQUIRE(constants.has_value());
        for (const f32 distance : {0.5F, 3.0F, 6.0F, 25.0F, 300.0F}) {
            CY_CHECK_NEAR(view_distance(*constants, depth_of(projection, distance)), distance,
                          2.0e-3F * distance);
        }
        // The cleared depth is the sky, infinitely far.
        CY_CHECK_EQ(view_distance(*constants, 0.0F), math::kInfinity);
    }
}

CY_TEST_CASE("a pinhole has no circle of confusion anywhere") {
    DofSettings settings = lens_settings();
    settings.lens.aperture = std::numeric_limits<f32>::infinity();
    const Expected<DofConstants, Error> constants =
        make_dof_constants(settings, perspective_view());
    CY_REQUIRE(constants.has_value());
    CY_CHECK_EQ(constants->lens[0], 0.0F);
    for (const f32 distance : {0.2F, 6.0F, 1000.0F, math::kInfinity}) {
        CY_CHECK_EQ(std::fabs(coc_radius_pixels(*constants, distance)), 0.0F);
    }
}

CY_TEST_CASE("the aperture is a circle or the polygon its blades inscribe in it") {
    DofSettings settings = lens_settings();
    const Expected<DofConstants, Error> round = make_dof_constants(settings, perspective_view());
    CY_REQUIRE(round.has_value());
    CY_CHECK_EQ(aperture_extent(*round, 1.234F), 1.0F);
    CY_CHECK_NEAR(aperture_area(0), math::kPi, 1.0e-6F);

    settings.blades = 6;
    const Expected<DofConstants, Error> hexagon = make_dof_constants(settings, perspective_view());
    CY_REQUIRE(hexagon.has_value());
    // A hexagon of unit circumradius reaches 1 at its corners and cos(30 degrees) at its edges'
    // midpoints; the first corner is at the blades' rotation.
    CY_CHECK_NEAR(aperture_extent(*hexagon, 0.0F), 1.0F, 1.0e-5F);
    CY_CHECK_NEAR(aperture_extent(*hexagon, math::kPi / 6.0F), std::cos(math::kPi / 6.0F), 1.0e-5F);
    CY_CHECK_NEAR(aperture_extent(*hexagon, math::kPi / 3.0F), 1.0F, 1.0e-5F);
    CY_CHECK_NEAR(aperture_extent(*hexagon, -math::kPi / 6.0F), std::cos(math::kPi / 6.0F),
                  1.0e-5F);
    // Its area: (3 sqrt 3) / 2.
    CY_CHECK_NEAR(aperture_area(6), 1.5F * std::sqrt(3.0F), 1.0e-5F);
    CY_CHECK_NEAR(hexagon->aperture[2], aperture_area(6), 1.0e-6F);

    // Four blades is not an aperture this gather draws, nor is seventeen.
    settings.blades = 4;
    CY_CHECK_FALSE(make_dof_constants(settings, perspective_view()).has_value());
    settings.blades = kMaxBlades + 1U;
    CY_CHECK_FALSE(make_dof_constants(settings, perspective_view()).has_value());
}

CY_TEST_CASE("the gather's taps cover the disc once") {
    for (u32 rings = 1; rings <= 8; ++rings) {
        const f32 radius = 5.5F;
        const u32 taps = gather_taps(rings);
        f32 area = 0.0F;
        f32 farthest = 0.0F;
        for (u32 index = 0; index < taps; ++index) {
            const GatherTap tap = gather_tap(index, rings, radius);
            area += tap.area;
            farthest = math::max(
                farthest, std::sqrt((tap.offset.x * tap.offset.x) + (tap.offset.y * tap.offset.y)));
        }
        // The areas are the disc's, and the outermost ring is half a spacing inside its rim.
        CY_CHECK_NEAR(area, math::kPi * radius * radius, 1.0e-3F * radius * radius);
        CY_CHECK_NEAR(farthest, radius * static_cast<f32>(rings) / (static_cast<f32>(rings) + 0.5F),
                      1.0e-4F);
    }
    CY_CHECK_EQ(gather_taps(6), 127U);
    CY_CHECK_EQ(gather_rings(0.2F, 6), 1U);
    CY_CHECK_EQ(gather_rings(3.2F, 6), 4U);
    CY_CHECK_EQ(gather_rings(30.0F, 6), 6U);
}

CY_TEST_CASE("a gather's outermost ring reaches the rim of the disc it gathers") {
    // Regression: the taps spanned the disc itself, which puts the outermost ring half a spacing
    // inside its rim, so every texel beyond it went unsampled and six blades drew the tap grid's
    // circle. Spanning the disc plus the reach's half-texel ramp puts that ring on the ramp's edge.
    for (const f32 radius : {1.0F, 3.4F, 6.84F, 12.0F}) {
        for (u32 rings = 1; rings <= 8; ++rings) {
            const f32 span = gather_span(radius, rings);
            const GatherTap outer = gather_tap(gather_taps(rings) - 1U, rings, span);
            const f32 reach =
                std::sqrt((outer.offset.x * outer.offset.x) + (outer.offset.y * outer.offset.y));
            CY_CHECK_NEAR(reach, radius + 0.5F, 1.0e-4F * (radius + 1.0F));
        }
    }
}

CY_TEST_CASE("the tiles are as wide as the largest radius") {
    const Expected<DofConstants, Error> constants =
        make_dof_constants(lens_settings(), perspective_view());
    CY_REQUIRE(constants.has_value());
    // 54 pixels of the target is 27 texels of the half-resolution layer.
    CY_CHECK_EQ(constants->tiles[0], 27U);
    CY_CHECK_EQ(constants->extent[2], 960U);
    CY_CHECK_EQ(constants->extent[3], 540U);
    CY_CHECK_EQ(constants->tiles[1], (960U + 26U) / 27U);
    CY_CHECK_EQ(constants->tiles[2], 20U);
    CY_CHECK_EQ(half_extent(271), 136U);
}

CY_TEST_CASE("a view or a lens the gather cannot draw is refused") {
    DofSettings settings = lens_settings();
    DofView empty = perspective_view();
    empty.height = 0;
    CY_CHECK_FALSE(make_dof_constants(settings, empty).has_value());
    DofSettings rings = settings;
    rings.max_rings = 0;
    CY_CHECK_FALSE(make_dof_constants(rings, perspective_view()).has_value());
    DofSettings radius = settings;
    radius.max_radius_fraction = 0.2F;
    CY_CHECK_FALSE(make_dof_constants(radius, perspective_view()).has_value());
    DofSettings behind = settings;
    behind.lens.focus_distance = 0.01F;
    CY_CHECK_FALSE(make_dof_constants(behind, perspective_view()).has_value());
}
