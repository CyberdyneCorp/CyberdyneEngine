#include <cy/rendering/sky/celestial.h>

#include <cy/core/math/math.h>

#include <cmath>

namespace cy::rendering::sky {
namespace {

[[nodiscard]] f32 radians_of(f32 degrees) noexcept {
    return degrees * (math::kPi / 180.0F);
}

[[nodiscard]] f32 wrap_unit(f32 value) noexcept {
    const f32 fraction = value - std::floor(value);
    return fraction < 0.0F ? fraction + 1.0F : fraction;
}

/// The direction to a body at a given hour angle and declination, in a frame where +Y is up, +X is
/// east and -Z is north. That frame is the engine's, and the transformation from the equatorial one
/// is the whole of the astronomy here.
[[nodiscard]] Vec3 horizon_direction(f32 latitude, f32 declination, f32 hour_angle) noexcept {
    const f32 sin_lat = std::sin(latitude);
    const f32 cos_lat = std::cos(latitude);
    const f32 sin_dec = std::sin(declination);
    const f32 cos_dec = std::cos(declination);
    const f32 cos_h = std::cos(hour_angle);
    const f32 sin_h = std::sin(hour_angle);

    const f32 altitude_sine = (sin_lat * sin_dec) + (cos_lat * cos_dec * cos_h);
    const f32 altitude = std::asin(math::clamp(altitude_sine, -1.0F, 1.0F));
    // Azimuth measured from north, increasing eastwards, which is the convention every almanac
    // uses and the one a reader checking this against one will expect.
    const f32 azimuth =
        std::atan2(-sin_h * cos_dec, (cos_lat * sin_dec) - (sin_lat * cos_dec * cos_h));

    const f32 cos_altitude = std::cos(altitude);
    return Vec3{cos_altitude * std::sin(azimuth), std::sin(altitude),
                -cos_altitude * std::cos(azimuth)};
}

}  // namespace

const char* time_domain_name(TimeDomain domain) noexcept {
    switch (domain) {
        case TimeDomain::Real:
            return "real";
        case TimeDomain::Gameplay:
            return "gameplay";
        case TimeDomain::Cinematic:
            return "cinematic";
        case TimeDomain::Custom:
            return "custom";
        case TimeDomain::Count:
            break;
    }
    return "unknown";
}

void advance_time_of_day(TimeOfDay& time, f32 delta_seconds) noexcept {
    if (time.paused || time.domain == TimeDomain::Custom) {
        // A custom domain advances only by an explicit assignment. Advancing it here would make a
        // scripted sky drift under whatever else was calling this.
        return;
    }
    const f32 per_day = math::max(time.seconds_per_day, 1.0e-3F);
    const f32 advanced = time.fraction + (delta_seconds / per_day);
    // The day count follows the wrap, including backwards: a cinematic domain may run in reverse
    // and a sky that lost a day doing so would drift out of season over a long shot.
    time.day_of_year += std::floor(advanced);
    time.fraction = wrap_unit(advanced);
    if (time.day_of_year >= 365.0F) {
        time.day_of_year -= 365.0F * std::floor(time.day_of_year / 365.0F);
    } else if (time.day_of_year < 0.0F) {
        time.day_of_year += 365.0F * (1.0F + std::floor(-time.day_of_year / 365.0F));
    }
}

CelestialState solve_celestial(const CelestialModel& model, const TimeOfDay& time) noexcept {
    const f32 latitude = radians_of(model.latitude_degrees);
    const f32 azimuth_offset = radians_of(model.azimuth_offset_degrees);

    // The declination: the axial tilt projected onto the year. Zero tilt gives a planet with no
    // seasons, which is the case a project asking for one wants to be able to reach.
    const f32 year = 2.0F * math::kPi * (time.day_of_year - 81.0F) / 365.0F;
    const f32 declination = radians_of(model.axial_tilt_degrees) * std::sin(year);

    // Local noon is fraction 0.5, so the hour angle runs from -pi at midnight through zero at noon.
    const f32 longitude = radians_of(model.longitude_degrees);
    const f32 hour_angle = ((time.fraction - 0.5F) * 2.0F * math::kPi) + longitude + azimuth_offset;

    CelestialState state;
    state.sun.direction = horizon_direction(latitude, declination, hour_angle);
    state.sun.illuminated_fraction = 1.0F;
    state.sun.above_horizon = state.sun.direction.y >= 0.0F;

    // The moon: the same sky, half a period out of step, with its own declination. This is a
    // convincing moon rather than a correct one, and `celestial.h` says the orbit is not modelled.
    const f32 period = math::max(model.moon_period_days, 1.0e-3F);
    const f32 phase =
        wrap_unit(((time.day_of_year + time.fraction) / period) + model.moon_phase_offset);
    const f32 moon_hour = hour_angle + (2.0F * math::kPi * phase) + math::kPi;
    const f32 moon_declination = declination * 0.9F;
    state.moon.direction = horizon_direction(latitude, moon_declination, moon_hour);
    // The illuminated fraction from the phase: new at 0, full at 0.5.
    state.moon.illuminated_fraction = 0.5F * (1.0F - std::cos(2.0F * math::kPi * phase));
    state.moon.above_horizon = state.moon.direction.y >= 0.0F;

    // Stars appear once the sun is well below the horizon. -6 degrees is civil twilight and -18 is
    // astronomical; the ramp between them is where stars actually come out.
    const f32 sun_altitude = std::asin(math::clamp(state.sun.direction.y, -1.0F, 1.0F));
    const f32 civil = radians_of(-6.0F);
    const f32 astronomical = radians_of(-18.0F);
    state.star_visibility =
        math::clamp((civil - sun_altitude) / (civil - astronomical), 0.0F, 1.0F);
    return state;
}

Vec3 light_travel_direction(const CelestialBody& body) noexcept {
    return -normalized_or(body.direction, Vec3{0.0F, 1.0F, 0.0F});
}

}  // namespace cy::rendering::sky
