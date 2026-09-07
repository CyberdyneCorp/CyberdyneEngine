#pragma once
// Celestial bodies and time of day. M7 task 10.4.
//
// `atmosphere-sky-and-clouds` — "Celestial bodies and time of day".
//
// ================================================================================================
// THE MODEL IS BYPASSABLE, AND THAT IS THE REQUIREMENT RATHER THAN A CONVENIENCE
// ================================================================================================
//
// "Realistic astronomy SHALL NOT be required; a project SHALL be able to place its sun by hand" and
// "WHEN a project places the sun directly for art direction THEN the celestial model SHALL be
// bypassable without losing atmosphere or sky."
//
// So `CelestialBody` carries a `direction` and everything else here is a way of COMPUTING one.
// `solve_celestial()` fills the directions from a `CelestialModel`; a project that sets them by
// hand never calls it, and `atmosphere.h` neither knows nor cares which happened. There is no path
// by which bypassing the model costs a project the sky.
//
// ================================================================================================
// TIME OF DAY IS ITS OWN DOMAIN
// ================================================================================================
//
// "Time of day SHALL advance on a declared time domain, mapping from gameplay, real, cinematic, or
// custom time, so that a day may be minutes or hours and may be paused or scripted independently of
// gameplay time."
//
// `TimeOfDay` therefore holds a rate and a clock and takes a delta in the SOURCE domain, converting
// once. A day that is twenty game minutes long is `seconds_per_day = 1200`, and pausing the sky is
// `paused = true` rather than a caller remembering not to advance it. Both are properties of this
// structure rather than of every call site, which is the difference between a declared domain and a
// convention.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

namespace cy::rendering::sky {

/// Which clock a time of day advances against. The mapping is the caller's — it passes the delta
/// from whichever clock this names — and recording it here is what makes "independently of gameplay
/// time" a declaration rather than a comment.
enum class TimeDomain : u8 {
    /// Wall-clock seconds. A day continues while the game is paused.
    Real = 0,
    /// The simulation's own time, which stops when it does.
    Gameplay,
    /// A sequence's time. `sequencing-and-cinematics` drives this one and it may run backwards.
    Cinematic,
    /// Advanced only by an explicit `set_time_of_day`. What a scripted event uses.
    Custom,
    Count,
};

[[nodiscard]] const char* time_domain_name(TimeDomain domain) noexcept;

/// The clock the sky runs on.
struct TimeOfDay {
    TimeDomain domain = TimeDomain::Gameplay;
    /// Seconds of the source domain per full day. 86,400 is real time; 1,200 is a twenty-minute
    /// day.
    f32 seconds_per_day = 1200.0F;
    /// The current time, in [0, 1). 0 is midnight, 0.5 is noon.
    f32 fraction = 0.5F;
    /// Which day of the year, in [0, 365]. Drives the sun's declination and therefore the seasons.
    f32 day_of_year = 172.0F;  // the June solstice
    bool paused = false;
};

/// Advance by `delta_seconds` of the domain's own clock. Wraps, and increments the day when it
/// does.
void advance_time_of_day(TimeOfDay& time, f32 delta_seconds) noexcept;

/// The celestial model: where on a planet, and how it turns.
struct CelestialModel {
    /// Degrees. Positive north.
    f32 latitude_degrees = 45.0F;
    /// Degrees. Only shifts local noon; kept because a project with a real map wants it.
    f32 longitude_degrees = 0.0F;
    /// The planet's axial tilt, in degrees. 23.44 is Earth's, and it is why there are seasons; zero
    /// gives a planet with none.
    f32 axial_tilt_degrees = 23.44F;
    /// A rotation applied to the whole celestial sphere, in degrees. What art direction reaches for
    /// when the sun is right but in the wrong part of the sky.
    f32 azimuth_offset_degrees = 0.0F;
    /// The moon's orbital period in days, and its phase offset. Only the direction and the phase
    /// are modelled; the moon's own orbit is not.
    f32 moon_period_days = 29.53F;
    f32 moon_phase_offset = 0.0F;
};

/// One body's state. `direction` points FROM the surface TOWARDS the body, which is the convention
/// every function in `atmosphere.h` takes and the opposite of a directional light's travel vector.
struct CelestialBody {
    Vec3 direction{0.0F, 1.0F, 0.0F};
    /// The fraction of the body that is lit, in [0, 1]. 1 for a star; the phase for a moon.
    f32 illuminated_fraction = 1.0F;
    /// Above the horizon. Not the same as `direction.y > 0`: a body exactly on the horizon is
    /// visible and its `y` is zero.
    bool above_horizon = true;
};

/// Where the sun and the moon are. The whole of the celestial model, and the function a project
/// that places its sun by hand never calls.
struct CelestialState {
    CelestialBody sun;
    CelestialBody moon;
    /// How visible the stars are, in [0, 1]: zero in daylight, one well after dusk. Drives nothing
    /// here — this module has no stars — and is reported because the sky composition consumes it.
    f32 star_visibility = 0.0F;
};

[[nodiscard]] CelestialState solve_celestial(const CelestialModel& model,
                                             const TimeOfDay& time) noexcept;

/// The travel direction of the directional light a body becomes: `-direction`. A named function
/// rather than a negation at each call site, because getting it backwards lights a scene from
/// exactly the wrong side and looks plausible until a shadow is compared with the sky.
[[nodiscard]] Vec3 light_travel_direction(const CelestialBody& body) noexcept;

}  // namespace cy::rendering::sky
