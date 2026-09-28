// SPDX-License-Identifier: MIT
#include <cy/rendering/depth_of_field/focus.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::rendering::depth_of_field {
namespace {

/// A circle of confusion's diameter, as `circle_of_confusion` gives it, to a radius in pixels.
constexpr f32 kDiameterToRadius = 0.5F;
/// `circle_of_confusion`'s own floors, restated so the device twin agrees with it everywhere.
constexpr f32 kMinimumFocalMm = 1.0F;
constexpr f32 kMinimumFNumber = 0.5F;
constexpr f32 kMinimumSensorMm = 1.0F;
constexpr f32 kMinimumDistance = 1.0e-4F;

}  // namespace

f32 focal_length_for_field_of_view(f32 vertical_fov_radians, f32 sensor_height_mm) noexcept {
    return (0.5F * sensor_height_mm) / std::tan(0.5F * vertical_fov_radians);
}

f32 aperture_area(u32 blades) noexcept {
    if (blades == 0) {
        return math::kPi;
    }
    const auto sides = static_cast<f32>(blades);
    return 0.5F * sides * std::sin(math::kTwoPi / sides);
}

Expected<DofConstants, Error> make_dof_constants(const DofSettings& settings,
                                                 const DofView& view) noexcept {
    if (view.width == 0 || view.height == 0) {
        return fail(ErrorCode::InvalidArgument, "depth of field: the view has no pixels");
    }
    if (settings.blades != 0 && (settings.blades < 5 || settings.blades > kMaxBlades)) {
        return fail(
            ErrorCode::OutOfRange,
            "depth of field: an aperture is a circle (0 blades) or 5 to 16 straight blades");
    }
    if (settings.max_rings == 0 || settings.max_rings > kMaxRings) {
        return fail(ErrorCode::OutOfRange, "depth of field: the gather takes 1 to 16 rings");
    }
    if (!(settings.max_radius_fraction > 0.0F) || settings.max_radius_fraction > 0.1F) {
        return fail(ErrorCode::OutOfRange,
                    "depth of field: the largest radius is a fraction of the image height in "
                    "(0, 0.1]");
    }
    const DepthOfFieldSettings& lens = settings.lens;
    const f32 focal = math::max(lens.focal_length_mm, kMinimumFocalMm) * 0.001F;
    if (!(lens.focus_distance > focal)) {
        return fail(ErrorCode::InvalidArgument,
                    "depth of field: the focus distance must be beyond the focal length");
    }
    if (!(lens.aperture > 0.0F) || !(lens.artistic_scale >= 0.0F)) {
        return fail(ErrorCode::InvalidArgument,
                    "depth of field: the f-number must be positive and the artistic scale not "
                    "negative");
    }

    DofConstants constants;
    constants.depth[0] = view.projection.at(2, 2);
    constants.depth[1] = view.projection.at(2, 3);
    constants.depth[2] = view.projection.at(3, 2);
    constants.depth[3] = view.projection.at(3, 3);

    // circle_of_confusion's expression with the distance factored out: A f / ((F - f) s), a
    // diameter as a fraction of the sensor, times H/2 for a radius in pixels. An f-number of
    // infinity makes the aperture diameter, and so K, exactly zero.
    const f32 focus = math::max(lens.focus_distance, focal * 1.01F);
    const f32 aperture_diameter = focal / math::max(lens.aperture, kMinimumFNumber);
    const f32 sensor = math::max(lens.sensor_height_mm, kMinimumSensorMm) * 0.001F;
    const f32 fraction = aperture_diameter * (focal / math::max(focus - focal, 1.0e-5F)) / sensor *
                         lens.artistic_scale;
    const auto height = static_cast<f32>(view.height);
    constants.lens[0] = kDiameterToRadius * fraction * height;
    constants.lens[1] = focus;
    constants.lens[2] = settings.max_radius_fraction * height;
    constants.lens[3] = kFocusBandPixels;

    constants.aperture[0] = static_cast<f32>(settings.blades);
    constants.aperture[1] = settings.blade_rotation;
    constants.aperture[2] = aperture_area(settings.blades);
    constants.aperture[3] = static_cast<f32>(settings.max_rings);

    constants.extent[0] = view.width;
    constants.extent[1] = view.height;
    constants.extent[2] = half_extent(view.width);
    constants.extent[3] = half_extent(view.height);

    // A tile at least as wide as the largest radius, in half-resolution texels, so a 3x3 block of
    // tiles holds every near-field texel whose blur can reach the centre tile.
    const auto tile = static_cast<u32>(std::ceil(constants.lens[2] * 0.5F));
    constants.tiles[0] = tile < 1U ? 1U : tile;
    constants.tiles[1] = (constants.extent[2] + constants.tiles[0] - 1U) / constants.tiles[0];
    constants.tiles[2] = (constants.extent[3] + constants.tiles[0] - 1U) / constants.tiles[0];
    return constants;
}

f32 view_distance(const DofConstants& constants, f32 depth) noexcept {
    const f32 denominator = constants.depth[0] - (depth * constants.depth[2]);
    if (!(depth > 0.0F) || denominator == 0.0F) {
        return math::kInfinity;
    }
    return (constants.depth[1] - (depth * constants.depth[3])) / denominator;
}

f32 coc_radius_pixels(const DofConstants& constants, f32 distance) noexcept {
    if (!(distance < math::kInfinity)) {
        return constants.lens[0];
    }
    const f32 clamped = math::max(distance, kMinimumDistance);
    return constants.lens[0] * (clamped - constants.lens[1]) / clamped;
}

f32 coc_radius_at_depth(const DofConstants& constants, f32 depth) noexcept {
    const f32 radius = coc_radius_pixels(constants, view_distance(constants, depth));
    return math::clamp(radius, -constants.lens[2], constants.lens[2]);
}

f32 aperture_extent(const DofConstants& constants, f32 angle_radians) noexcept {
    const f32 blades = constants.aperture[0];
    if (blades < 1.0F) {
        return 1.0F;
    }
    const f32 sector = math::kTwoPi / blades;
    const f32 turned = angle_radians - constants.aperture[1];
    const f32 within = turned - (sector * std::floor(turned / sector));
    return std::cos(0.5F * sector) / std::cos(within - (0.5F * sector));
}

u32 gather_rings(f32 radius, u32 max_rings) noexcept {
    const f32 wanted = std::ceil(math::max(radius, 1.0F));
    const auto rings = static_cast<u32>(wanted);
    return rings < max_rings ? rings : max_rings;
}

f32 gather_span(f32 radius, u32 rings) noexcept {
    const auto count = static_cast<f32>(rings);
    return (radius + 0.5F) * (count + 0.5F) / count;
}

GatherTap gather_tap(u32 index, u32 rings, f32 radius) noexcept {
    const f32 spacing = radius / (static_cast<f32>(rings) + 0.5F);
    GatherTap tap;
    if (index == 0) {
        // The centre stands for the disc of radius half a spacing.
        tap.area = 0.25F * math::kPi * spacing * spacing;
        return tap;
    }
    // Ring k starts at tap 1 + 3k(k - 1) and holds 6k taps, each a sixth-k share of the annulus
    // [(k - 1/2), (k + 1/2)] spacings: 2 pi k spacing^2 / 6k.
    u32 ring = 1;
    while (index >= 1U + (3U * ring * (ring + 1U))) {
        ++ring;
    }
    const u32 first = 1U + (3U * ring * (ring - 1U));
    const u32 count = 6U * ring;
    // Alternate rings are turned half a tap so no two rings line up along a spoke.
    const f32 turn = (static_cast<f32>(index - first) + ((ring & 1U) != 0U ? 0.0F : 0.5F)) /
                     static_cast<f32>(count);
    const f32 angle = math::kTwoPi * turn;
    const f32 distance = static_cast<f32>(ring) * spacing;
    tap.offset = Vec2{distance * std::cos(angle), distance * std::sin(angle)};
    tap.area = (math::kPi / 3.0F) * spacing * spacing;
    return tap;
}

}  // namespace cy::rendering::depth_of_field
