#include <cy/rendering/sky/atmosphere.h>

#include <cy/core/math/math.h>

#include "internal.h"

#include <cmath>

namespace cy::rendering::sky {

// The ray-sphere and density arithmetic these functions are written in lives in `internal.h`,
// because M10's tables, clouds and composition need the same arithmetic and a second copy of
// `entry_distance()` would be a second place for the below-the-horizon defect this module's
// README records to come back.
using detail::density_at;
using detail::entry_distance;
using detail::exit_distance;
using detail::exp3;
using detail::ozone_density_at;

Atmosphere earth_atmosphere() noexcept {
    return Atmosphere{};
}

Atmosphere thin_dusty_atmosphere() noexcept {
    Atmosphere atmosphere;
    atmosphere.planet_radius = 3390000.0F;      // Mars-sized
    atmosphere.atmosphere_radius = 3440000.0F;  // a 50 km shell
    // A hundredth of Earth's Rayleigh: a thin atmosphere scatters almost nothing at short
    // wavelengths, which is why its daytime sky is not blue.
    atmosphere.rayleigh_scattering = Vec3{0.06e-6F, 0.14e-6F, 0.33e-6F};
    atmosphere.rayleigh_scale_height = 11000.0F;
    // And a great deal of Mie: suspended dust, which is grey-red and forward scattering.
    atmosphere.mie_scattering = 20.0e-6F;
    atmosphere.mie_extinction = 24.0e-6F;
    atmosphere.mie_anisotropy = 0.62F;
    atmosphere.mie_scale_height = 9000.0F;
    atmosphere.ozone_absorption = Vec3{0.0F, 0.0F, 0.0F};
    atmosphere.ground_albedo = Vec3{0.28F, 0.18F, 0.11F};
    atmosphere.stellar_illuminance = 55000.0F;  // further from its star
    return atmosphere;
}

Status validate_atmosphere(const Atmosphere& atmosphere) noexcept {
    if (!(atmosphere.planet_radius > 0.0F) ||
        atmosphere.atmosphere_radius <= atmosphere.planet_radius) {
        return fail(ErrorCode::InvalidArgument,
                    "validate_atmosphere: the atmosphere's top must be above the planet's surface");
    }
    if (!(atmosphere.rayleigh_scale_height > 0.0F) || !(atmosphere.mie_scale_height > 0.0F)) {
        return fail(ErrorCode::InvalidArgument,
                    "validate_atmosphere: a scale height is the altitude at which density falls by "
                    "1/e and must be positive");
    }
    if (atmosphere.mie_scattering < 0.0F || atmosphere.mie_extinction < atmosphere.mie_scattering) {
        return fail(ErrorCode::InvalidArgument,
                    "validate_atmosphere: Mie extinction is scattering plus absorption and cannot "
                    "be below the scattering it contains");
    }
    if (atmosphere.mie_anisotropy <= -1.0F || atmosphere.mie_anisotropy >= 1.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "validate_atmosphere: the phase anisotropy is in the open interval (-1, 1)");
    }
    if (atmosphere.rayleigh_scattering.x < 0.0F || atmosphere.rayleigh_scattering.y < 0.0F ||
        atmosphere.rayleigh_scattering.z < 0.0F || atmosphere.ozone_absorption.x < 0.0F ||
        atmosphere.ozone_absorption.y < 0.0F || atmosphere.ozone_absorption.z < 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "validate_atmosphere: a scattering or absorption coefficient cannot be "
                    "negative");
    }
    if (!(atmosphere.stellar_illuminance >= 0.0F) || !(atmosphere.stellar_angular_radius > 0.0F)) {
        return fail(ErrorCode::InvalidArgument,
                    "validate_atmosphere: the star needs a non-negative illuminance and a positive "
                    "angular radius");
    }
    return {};
}

Atmosphere blend_atmospheres(const Atmosphere& from, const Atmosphere& to, f32 t) noexcept {
    const f32 amount = math::clamp(t, 0.0F, 1.0F);
    const auto mix = [amount](f32 a, f32 b) { return a + ((b - a) * amount); };
    const auto mix3 = [amount](Vec3 a, Vec3 b) { return lerp(a, b, amount); };

    Atmosphere result;
    result.planet_radius = mix(from.planet_radius, to.planet_radius);
    result.atmosphere_radius = mix(from.atmosphere_radius, to.atmosphere_radius);
    result.rayleigh_scattering = mix3(from.rayleigh_scattering, to.rayleigh_scattering);
    result.rayleigh_scale_height = mix(from.rayleigh_scale_height, to.rayleigh_scale_height);
    result.mie_scattering = mix(from.mie_scattering, to.mie_scattering);
    result.mie_extinction = mix(from.mie_extinction, to.mie_extinction);
    result.mie_anisotropy = mix(from.mie_anisotropy, to.mie_anisotropy);
    result.mie_scale_height = mix(from.mie_scale_height, to.mie_scale_height);
    result.ozone_absorption = mix3(from.ozone_absorption, to.ozone_absorption);
    result.ozone_center = mix(from.ozone_center, to.ozone_center);
    result.ozone_width = mix(from.ozone_width, to.ozone_width);
    result.ground_albedo = mix3(from.ground_albedo, to.ground_albedo);
    result.stellar_illuminance = mix(from.stellar_illuminance, to.stellar_illuminance);
    result.stellar_angular_radius = mix(from.stellar_angular_radius, to.stellar_angular_radius);
    result.multiple_scattering_factor =
        mix(from.multiple_scattering_factor, to.multiple_scattering_factor);
    return result;
}

f32 rayleigh_phase(f32 cos_theta) noexcept {
    return (3.0F / (16.0F * math::kPi)) * (1.0F + (cos_theta * cos_theta));
}

f32 mie_phase(f32 cos_theta, f32 anisotropy) noexcept {
    const f32 g = math::clamp(anisotropy, -0.99F, 0.99F);
    const f32 g2 = g * g;
    const f32 numerator = 3.0F * (1.0F - g2) * (1.0F + (cos_theta * cos_theta));
    const f32 base = 1.0F + g2 - (2.0F * g * cos_theta);
    const f32 denominator =
        8.0F * math::kPi * (2.0F + g2) * base * std::sqrt(math::max(base, 1e-6F));
    return numerator / math::max(denominator, 1.0e-12F);
}

Vec3 extinction_at(const Atmosphere& atmosphere, f32 altitude_metres) noexcept {
    const f32 rayleigh = density_at(altitude_metres, atmosphere.rayleigh_scale_height);
    const f32 mie = density_at(altitude_metres, atmosphere.mie_scale_height);
    const f32 ozone = ozone_density_at(atmosphere, altitude_metres);
    return atmosphere.rayleigh_scattering * rayleigh +
           Vec3{atmosphere.mie_extinction, atmosphere.mie_extinction, atmosphere.mie_extinction} *
               mie +
           atmosphere.ozone_absorption * ozone;
}

f32 altitude_of(const Atmosphere& atmosphere, Vec3 position) noexcept {
    return length(position) - atmosphere.planet_radius;
}

Vec3 ground_position(const Atmosphere& atmosphere, f32 altitude_metres) noexcept {
    return Vec3{0.0F, atmosphere.planet_radius + math::max(altitude_metres, 0.0F), 0.0F};
}

Vec3 transmittance(const Atmosphere& atmosphere, Vec3 from, Vec3 direction, u32 steps) noexcept {
    const Vec3 unit = normalized_or(direction, Vec3{0.0F, 1.0F, 0.0F});
    // A ray that meets the ground is fully occluded: nothing reaches the top of the atmosphere
    // through a planet. Reporting zero rather than integrating on past the surface is what stops
    // the sun lighting a scene from below the horizon.
    if (entry_distance(from, unit, atmosphere.planet_radius) >= 0.0F) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    const f32 top = exit_distance(from, unit, atmosphere.atmosphere_radius);
    if (top <= 0.0F) {
        return Vec3{1.0F, 1.0F, 1.0F};
    }

    const u32 count = math::max(steps, 2U);
    const f32 step = top / static_cast<f32>(count);
    Vec3 optical_depth{0.0F, 0.0F, 0.0F};
    for (u32 index = 0; index < count; ++index) {
        const Vec3 sample = from + unit * (step * (static_cast<f32>(index) + 0.5F));
        optical_depth =
            optical_depth + extinction_at(atmosphere, altitude_of(atmosphere, sample)) * step;
    }
    return exp3(optical_depth);
}

Vec3 sky_radiance(const Atmosphere& atmosphere, Vec3 view_position, Vec3 view_direction,
                  Vec3 sun_direction, u32 steps) noexcept {
    const Vec3 view = normalized_or(view_direction, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 sun = normalized_or(sun_direction, Vec3{0.0F, 1.0F, 0.0F});

    const f32 ground = entry_distance(view_position, view, atmosphere.planet_radius);
    const f32 top = exit_distance(view_position, view, atmosphere.atmosphere_radius);
    const f32 length_along = ground >= 0.0F ? ground : top;
    if (length_along <= 0.0F) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }

    const f32 cos_theta = dot(view, sun);
    const f32 rayleigh_p = rayleigh_phase(cos_theta);
    const f32 mie_p = mie_phase(cos_theta, atmosphere.mie_anisotropy);

    const u32 count = math::max(steps, 4U);
    const f32 step = length_along / static_cast<f32>(count);
    Vec3 optical_depth{0.0F, 0.0F, 0.0F};
    Vec3 scattered{0.0F, 0.0F, 0.0F};

    for (u32 index = 0; index < count; ++index) {
        const Vec3 sample = view_position + view * (step * (static_cast<f32>(index) + 0.5F));
        const f32 altitude = altitude_of(atmosphere, sample);
        const f32 rayleigh_density = density_at(altitude, atmosphere.rayleigh_scale_height);
        const f32 mie_density = density_at(altitude, atmosphere.mie_scale_height);

        optical_depth = optical_depth + extinction_at(atmosphere, altitude) * step;
        const Vec3 view_transmittance = exp3(optical_depth);
        const Vec3 sun_transmittance = transmittance(atmosphere, sample, sun, 12);

        const Vec3 rayleigh_term = atmosphere.rayleigh_scattering * (rayleigh_density * rayleigh_p);
        const Vec3 mie_term{atmosphere.mie_scattering * mie_density * mie_p,
                            atmosphere.mie_scattering * mie_density * mie_p,
                            atmosphere.mie_scattering * mie_density * mie_p};

        // The isotropic multiple-scattering stand-in: the same scattering coefficients with a
        // uniform phase. It is not a second-order table and the header says so; what it buys is a
        // horizon and a shadowed slope that are not black. `tables.h`'s
        // `MultipleScatteringTable` is the table that replaces it.
        //
        // IT IS ATTENUATED BY THE SUN'S OWN TRANSMITTANCE, AND M10 ADDED THAT FACTOR. Without it
        // the term is added wherever the view ray passes, whether or not any sunlight reaches that
        // air — so a sun twenty degrees BELOW the horizon still lit the sky, and the measured
        // consequence was 378 nits at the zenith and 2 679 at the horizon at midnight, which is
        // daylight brightness in a scene that is supposed to be dark. Multiply-scattered light is
        // still light that came from the star, and air the star cannot reach does not have any.
        // `test_sky_tables.cpp`'s midnight case is the regression test.
        const Vec3 multiple = cwise_mul((atmosphere.rayleigh_scattering * rayleigh_density +
                                         Vec3{atmosphere.mie_scattering, atmosphere.mie_scattering,
                                              atmosphere.mie_scattering} *
                                             mie_density),
                                        sun_transmittance) *
                              (atmosphere.multiple_scattering_factor / (4.0F * math::kPi));

        scattered = scattered +
                    cwise_mul(view_transmittance,
                              cwise_mul(sun_transmittance, rayleigh_term + mie_term) + multiple) *
                        step;
    }

    Vec3 radiance = scattered * atmosphere.stellar_illuminance;

    if (ground > 0.0F) {
        // The ground the ray hit — strictly positive, because a ray that starts on the surface and
        // immediately meets it has nothing between the eye and the hit to integrate. lit by the sun
        // through the atmosphere and seen through what is left of the view path. This is the only
        // route by which a planet's surface brightens its own sky here, and it is why
        // `ground_albedo` is a parameter rather than a constant.
        const Vec3 hit = view_position + view * ground;
        const Vec3 normal = normalize(hit);
        const f32 lambert = math::max(0.0F, dot(normal, sun));
        const Vec3 sun_at_ground = transmittance(atmosphere, hit, sun, 16);
        const Vec3 ground_radiance = cwise_mul(atmosphere.ground_albedo, sun_at_ground) *
                                     (lambert * atmosphere.stellar_illuminance / math::kPi);
        radiance = radiance + cwise_mul(exp3(optical_depth), ground_radiance);
    }
    return radiance;
}

Vec3 stellar_radiance(const Atmosphere& atmosphere, Vec3 view_position, Vec3 view_direction,
                      Vec3 sun_direction) noexcept {
    const Vec3 view = normalized_or(view_direction, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 sun = normalized_or(sun_direction, Vec3{0.0F, 1.0F, 0.0F});
    const f32 cos_theta = math::clamp(dot(view, sun), -1.0F, 1.0F);
    if (cos_theta < std::cos(atmosphere.stellar_angular_radius)) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    // Illuminance divided by the disc's solid angle is its radiance, and the atmosphere takes its
    // share on the way. A renderer that instead drew a fixed-brightness disc would have a sun that
    // does not redden at sunset while the sky around it does.
    const f32 solid_angle = 2.0F * math::kPi * (1.0F - std::cos(atmosphere.stellar_angular_radius));
    const Vec3 through = transmittance(atmosphere, view_position, sun, 40);
    return through * (atmosphere.stellar_illuminance / math::max(solid_angle, 1.0e-9F));
}

Vec3 sun_illuminance(const Atmosphere& atmosphere, Vec3 view_position,
                     Vec3 sun_direction) noexcept {
    const Vec3 sun = normalized_or(sun_direction, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 through = transmittance(atmosphere, view_position, sun, 40);
    return through * atmosphere.stellar_illuminance;
}

AerialPerspective aerial_perspective(const Atmosphere& atmosphere, Vec3 from, Vec3 to,
                                     Vec3 sun_direction, u32 steps) noexcept {
    AerialPerspective result;
    const Vec3 offset = to - from;
    const f32 distance = length(offset);
    if (distance <= 1.0e-3F) {
        return result;
    }
    const Vec3 view = offset / distance;
    const Vec3 sun = normalized_or(sun_direction, Vec3{0.0F, 1.0F, 0.0F});
    const f32 cos_theta = dot(view, sun);
    const f32 rayleigh_p = rayleigh_phase(cos_theta);
    const f32 mie_p = mie_phase(cos_theta, atmosphere.mie_anisotropy);

    const u32 count = math::max(steps, 2U);
    const f32 step = distance / static_cast<f32>(count);
    Vec3 optical_depth{0.0F, 0.0F, 0.0F};
    Vec3 scattered{0.0F, 0.0F, 0.0F};
    for (u32 index = 0; index < count; ++index) {
        const Vec3 sample = from + view * (step * (static_cast<f32>(index) + 0.5F));
        const f32 altitude = altitude_of(atmosphere, sample);
        optical_depth = optical_depth + extinction_at(atmosphere, altitude) * step;
        const Vec3 view_transmittance = exp3(optical_depth);
        const Vec3 sun_transmittance = transmittance(atmosphere, sample, sun, 8);
        const f32 rayleigh_density = density_at(altitude, atmosphere.rayleigh_scale_height);
        const f32 mie_density = density_at(altitude, atmosphere.mie_scale_height);
        const Vec3 rayleigh_term = atmosphere.rayleigh_scattering * (rayleigh_density * rayleigh_p);
        const f32 mie_value = atmosphere.mie_scattering * mie_density * mie_p;
        scattered = scattered +
                    cwise_mul(view_transmittance,
                              cwise_mul(sun_transmittance,
                                        rayleigh_term + Vec3{mie_value, mie_value, mie_value})) *
                        step;
    }
    result.transmittance = exp3(optical_depth);
    result.in_scattering = scattered * atmosphere.stellar_illuminance;
    return result;
}

AerialPerspective stylise(const AerialPerspective& physical, const StylisedDistance& style,
                          f32 distance_metres) noexcept {
    const f32 strength = math::clamp(style.strength, 0.0F, 1.0F);
    if (strength <= 0.0F) {
        return physical;
    }
    const f32 half = math::max(style.half_distance, 1.0F);
    // Exponential in half-distances: the one shape a stylised fog is always asked for, expressed so
    // that `half_distance` means what it says.
    const f32 remaining = std::exp2(-math::max(distance_metres, 0.0F) / half);
    const Vec3 stylised_transmittance{remaining, remaining, remaining};

    // The stylised in-scattering is normalised against the physical term's own magnitude, so
    // switching the override on does not change the frame's exposure. A stylisation that also
    // rescaled the image would be indistinguishable from a bug.
    const f32 scale =
        math::max(physical.in_scattering.x + physical.in_scattering.y + physical.in_scattering.z,
                  0.0F) /
        3.0F;
    const Vec3 stylised_scatter = style.tint * (scale * (1.0F - remaining));

    AerialPerspective result;
    result.transmittance = lerp(physical.transmittance, stylised_transmittance, strength);
    result.in_scattering = lerp(physical.in_scattering, stylised_scatter, strength);
    return result;
}

}  // namespace cy::rendering::sky
