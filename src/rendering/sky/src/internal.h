#pragma once
// The ray-sphere and density arithmetic every file in this module needs, in ONE place.
//
// It was `atmosphere.cpp`'s anonymous namespace until M10 needed the same functions in
// `tables.cpp`, `clouds.cpp` and `composition.cpp`. Copying them would have copied
// `entry_distance()`, and `entry_distance()` carries a fix for a defect the module's README
// records: a ray starting exactly on the surface and pointing down was declared to MISS the planet,
// so the integration ran straight through it and a sun 30 degrees below the horizon delivered
// 5e-36 lux instead of none. A second copy is a second place for that to come back.
//
// Internal to the module: this header is under `src/`, not `include/`, so nothing outside can name
// it and the public surface is unchanged by its existence.

#include <cy/core/math/math.h>
#include <cy/core/math/vec.h>
#include <cy/rendering/sky/atmosphere.h>

#include <cmath>

namespace cy::rendering::sky::detail {

/// Where a ray from `origin` in `direction` leaves a sphere of `radius` centred on the coordinate
/// system's origin, or -1 when it never does. Only the FAR root is wanted: the view ray starts
/// inside the atmosphere and leaves through the top.
[[nodiscard]] inline f32 exit_distance(Vec3 origin, Vec3 direction, f32 radius) noexcept {
    const f32 b = dot(origin, direction);
    const f32 c = length_squared(origin) - (radius * radius);
    const f32 discriminant = (b * b) - c;
    if (discriminant < 0.0F) {
        return -1.0F;
    }
    return -b + std::sqrt(discriminant);
}

/// Where a ray first meets a sphere, or -1 when it misses or the sphere is behind.
///
/// THE TEST IS ON THE CLOSEST APPROACH, NOT ON THE ROOT'S SIGN. A ray starting exactly ON the
/// surface and pointing down has a first root of exactly zero, which `root > 0` rejects. Not zero
/// is not zero, and the thing that eventually notices is a tone mapper.
[[nodiscard]] inline f32 entry_distance(Vec3 origin, Vec3 direction, f32 radius) noexcept {
    const f32 b = dot(origin, direction);
    if (b >= 0.0F) {
        return -1.0F;  // heading away from the centre: it cannot meet a sphere it is outside of
    }
    const f32 closest_squared = length_squared(origin) - (b * b);
    if (closest_squared >= radius * radius) {
        return -1.0F;
    }
    const f32 root =
        -b - std::sqrt(math::max((b * b) - (length_squared(origin) - (radius * radius)), 0.0F));
    return math::max(root, 0.0F);
}

/// `exp(-v)` per channel. Named for what it is used for — turning an optical depth into a
/// transmittance — rather than for the arithmetic, because the negation is easy to lose.
[[nodiscard]] inline Vec3 exp3(Vec3 optical_depth) noexcept {
    return Vec3{std::exp(-optical_depth.x), std::exp(-optical_depth.y), std::exp(-optical_depth.z)};
}

[[nodiscard]] inline f32 density_at(f32 altitude, f32 scale_height) noexcept {
    return std::exp(-math::max(altitude, 0.0F) / math::max(scale_height, 1.0F));
}

/// Ozone's tent: one at the layer's centre, zero at its edges, and it does not fall off with
/// altitude the way a scale-height gas does. That shape is why a clear sky stays blue at twilight
/// rather than going grey — the long horizon path passes through the layer and loses its greens.
[[nodiscard]] inline f32 ozone_density_at(const Atmosphere& atmosphere, f32 altitude) noexcept {
    const f32 width = math::max(atmosphere.ozone_width, 1.0F);
    const f32 offset = std::fabs(altitude - atmosphere.ozone_center) / width;
    return math::max(0.0F, 1.0F - offset);
}

/// The scattering coefficient at an altitude: Rayleigh plus Mie, per channel. The counterpart of
/// the public `extinction_at()`, which also carries absorption — the two differ by ozone and by
/// Mie's absorbing share, and mixing them up makes haze that emits light.
[[nodiscard]] inline Vec3 scattering_at(const Atmosphere& atmosphere, f32 altitude) noexcept {
    const f32 rayleigh = density_at(altitude, atmosphere.rayleigh_scale_height);
    const f32 mie = atmosphere.mie_scattering * density_at(altitude, atmosphere.mie_scale_height);
    return (atmosphere.rayleigh_scattering * rayleigh) + Vec3{mie, mie, mie};
}

/// Whether two atmospheres are the same set of numbers. Used to decide whether a table has to be
/// regenerated — "Tables SHALL be regenerated only when the parameters they depend on change" —
/// and it compares EVERY parameter, because a comparison that omitted one would be a table that
/// silently described a different planet.
[[nodiscard]] inline bool same_atmosphere(const Atmosphere& a, const Atmosphere& b) noexcept {
    return a.planet_radius == b.planet_radius && a.atmosphere_radius == b.atmosphere_radius &&
           a.rayleigh_scattering == b.rayleigh_scattering &&
           a.rayleigh_scale_height == b.rayleigh_scale_height &&
           a.mie_scattering == b.mie_scattering && a.mie_extinction == b.mie_extinction &&
           a.mie_anisotropy == b.mie_anisotropy && a.mie_scale_height == b.mie_scale_height &&
           a.ozone_absorption == b.ozone_absorption && a.ozone_center == b.ozone_center &&
           a.ozone_width == b.ozone_width && a.ground_albedo == b.ground_albedo &&
           a.stellar_illuminance == b.stellar_illuminance &&
           a.stellar_angular_radius == b.stellar_angular_radius &&
           a.multiple_scattering_factor == b.multiple_scattering_factor;
}

}  // namespace cy::rendering::sky::detail
