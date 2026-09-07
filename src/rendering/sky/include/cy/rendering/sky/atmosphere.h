#pragma once
// A participating atmosphere, parameterised by physical quantities, and the tables sky evaluation
// samples rather than ray marches. M7 task 10.4.
//
// `atmosphere-sky-and-clouds` at **Seed**. The milestone's task says "an analytic sky sufficient
// for GI's sky term", and `sky_light.h` is where that sufficiency is made a measurement rather than
// a claim.
//
// ================================================================================================
// THE SKY FOLLOWS FROM THE PARAMETERS. THERE IS NO TINTED PRESET ANYWHERE IN THIS MODULE
// ================================================================================================
//
// "The model SHALL support atmospheres that are not Earth's — thin, dusty, dense, or chemically
// different — so that a planet's sky follows from its parameters rather than from a tinted preset."
//
// The enforcement is that `sky_radiance()` takes an `Atmosphere` and a direction and has no other
// input: there is no colour constant in this file, and `earth_atmosphere()` is one named set of
// numbers among the many a project can write. The Rayleigh coefficients ARE why the sky is blue —
// they go as 1/lambda^4, so the blue channel scatters six times as strongly as the red — and a
// project that changes them gets a different colour because the arithmetic changed, not because a
// preset was swapped.
//
// ================================================================================================
// WHAT IS AND IS NOT MODELLED, SAID PLAINLY
// ================================================================================================
//
// MODELLED: Rayleigh and Mie single scattering, ozone absorption on its own tent-shaped profile,
// transmittance along a view ray, the Cornette-Shanks phase function, ground albedo as a constant
// ambient term, and the sun's own disc. That set is what the requirement calls a physically based
// atmosphere at Seed, and it is what a GI sky term integrates.
//
// NOT MODELLED, and each is a named gap rather than a silence:
//
//   * MULTIPLE SCATTERING. The requirement's table list includes it; this module computes an
//     ISOTROPIC MULTIPLE-SCATTERING APPROXIMATION (`Atmosphere::multiple_scattering_factor`) rather
//     than the second-order table. The visible consequence is a sky that is slightly too dark near
//     the horizon at sunset and a shadowed side of a mountain that is too blue. Closing it is a
//     second table with the same shape as the transmittance one.
//   * CLOUDS, AURORAE AND STARS. `atmosphere-sky-and-clouds` requires all three and none is here;
//     they are the capability's Working tier, not its Seed.
//   * A TABLE THAT UPDATES INCREMENTALLY AS THE SUN MOVES. `SkyViewTable` is rebuilt in full when
//     the sun moves past `kSunMovementThreshold`, and the requirement asks for incremental
//     regeneration. The cost is measured and reported (`SkyTableStats`), which is the half of that
//     requirement this tier does honour.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

namespace cy::rendering::sky {

/// An atmosphere, in physical units. Lengths are METRES and coefficients are PER METRE, which is
/// the one convention choice in this file and is made here because the alternative — kilometres,
/// as most published parameter sets use — puts a factor of 1000 in every call site.
struct Atmosphere {
    /// The planet's radius, in metres. Earth is 6,360 km.
    f32 planet_radius = 6360000.0F;
    /// The top of the atmosphere, in metres from the planet's centre. Earth is 6,420 km.
    f32 atmosphere_radius = 6420000.0F;

    /// Rayleigh scattering at sea level, per metre, per channel. THIS IS WHY THE SKY IS BLUE: the
    /// coefficients go as 1/lambda^4, so blue scatters about six times as strongly as red.
    Vec3 rayleigh_scattering{5.802e-6F, 13.558e-6F, 33.1e-6F};
    /// The height over which Rayleigh density falls by 1/e. Earth is 8 km.
    f32 rayleigh_scale_height = 8000.0F;

    /// Mie scattering at sea level, per metre. Aerosols: grey, because the particles are large
    /// compared with the wavelength.
    f32 mie_scattering = 3.996e-6F;
    /// Mie extinction is scattering plus absorption; the ratio is what makes haze grey rather than
    /// merely bright.
    f32 mie_extinction = 4.44e-6F;
    /// Cornette-Shanks anisotropy, in (-1, 1). Positive is forward scattering, which is what makes
    /// the sky bright around the sun.
    f32 mie_anisotropy = 0.8F;
    /// Earth is 1.2 km.
    f32 mie_scale_height = 1200.0F;

    /// Ozone absorption, per metre. It has no scale height: it sits in a layer, which is why a
    /// clear sky stays blue at twilight instead of going grey.
    Vec3 ozone_absorption{0.650e-6F, 1.881e-6F, 0.085e-6F};
    /// The layer's centre and half width, in metres. Earth's is centred at 25 km, 15 km wide.
    f32 ozone_center = 25000.0F;
    f32 ozone_width = 15000.0F;

    /// The ground's albedo, which feeds the sky from below. A planet with a bright surface has a
    /// brighter sky, and this is the only path by which that is true here.
    Vec3 ground_albedo{0.1F, 0.1F, 0.1F};

    /// The star's illuminance at the top of the atmosphere, in lux. Earth's sun is about 128,000.
    f32 stellar_illuminance = 128000.0F;
    /// The star's angular radius, in radians. The sun's is 0.00465.
    f32 stellar_angular_radius = 0.00465F;

    /// The isotropic multiple-scattering approximation's strength, in [0, 1]. See the header's
    /// second section: this stands in for a second-order table that is not here, and a project
    /// that wants the sky darker at the horizon lowers it.
    f32 multiple_scattering_factor = 0.35F;
};

/// Earth, at sea level, at noon. One named set of numbers, not a privileged one.
[[nodiscard]] Atmosphere earth_atmosphere() noexcept;

/// A thin dusty atmosphere: less Rayleigh, more Mie, no ozone. Here because the requirement's own
/// scenario is "a project sets a dusty thin atmosphere", and a scenario with no example in the tree
/// is a scenario nobody runs.
[[nodiscard]] Atmosphere thin_dusty_atmosphere() noexcept;

/// Refuse an atmosphere whose radii or coefficients cannot describe one: a top below the surface,
/// a negative coefficient, an anisotropy outside (-1, 1), a Mie scattering above its own
/// extinction.
[[nodiscard]] Status validate_atmosphere(const Atmosphere& atmosphere) noexcept;

/// Blend two atmospheres. "The change SHALL be a parameter transition rather than a preset switch"
/// — so this interpolates every parameter, and there is no function here that swaps one for
/// another.
[[nodiscard]] Atmosphere blend_atmospheres(const Atmosphere& from, const Atmosphere& to,
                                           f32 t) noexcept;

/// Rayleigh's phase function. Normalised so it integrates to one over the sphere.
[[nodiscard]] f32 rayleigh_phase(f32 cos_theta) noexcept;

/// Cornette-Shanks, which is the Henyey-Greenstein form corrected to match Mie's forward peak.
[[nodiscard]] f32 mie_phase(f32 cos_theta, f32 anisotropy) noexcept;

/// The extinction coefficient at an altitude, per metre, per channel. Rayleigh plus Mie plus ozone.
[[nodiscard]] Vec3 extinction_at(const Atmosphere& atmosphere, f32 altitude_metres) noexcept;

/// The fraction of light surviving a path from `from` to the top of the atmosphere along
/// `direction`, per channel. Both are in planet-centred coordinates, in metres.
///
/// Ray marched here; `TransmittanceTable` is what a frame samples.
[[nodiscard]] Vec3 transmittance(const Atmosphere& atmosphere, Vec3 from, Vec3 direction,
                                 u32 steps = 40) noexcept;

/// The sky's radiance in a direction, by single scattering along the view ray, plus the isotropic
/// multiple-scattering approximation and the ground's contribution.
///
/// `view_position` is planet-centred, in metres; `view_direction` and `sun_direction` are unit
/// vectors in the same frame. The result is in the same units as `stellar_illuminance` divided by
/// steradians — that is, radiance in nits when the illuminance is in lux.
[[nodiscard]] Vec3 sky_radiance(const Atmosphere& atmosphere, Vec3 view_position,
                                Vec3 view_direction, Vec3 sun_direction, u32 steps = 32) noexcept;

/// The star's own disc, added to `sky_radiance` when the view ray hits it. Separate because a sky
/// view table cannot resolve a disc 0.5 degrees across and a renderer draws it as its own element.
[[nodiscard]] Vec3 stellar_radiance(const Atmosphere& atmosphere, Vec3 view_position,
                                    Vec3 view_direction, Vec3 sun_direction) noexcept;

/// The sunlight reaching the ground: illuminance in lux, per channel, after the atmosphere has
/// taken its share. The DIRECTIONAL LIGHT the renderer places for the sun, so its colour follows
/// from the atmosphere rather than from a curve somebody tuned.
[[nodiscard]] Vec3 sun_illuminance(const Atmosphere& atmosphere, Vec3 view_position,
                                   Vec3 sun_direction) noexcept;

/// The height above the planet's surface for a planet-centred position, in metres. Negative below
/// the surface, which a caller inside terrain will produce and which the model clamps.
[[nodiscard]] f32 altitude_of(const Atmosphere& atmosphere, Vec3 position) noexcept;

/// A planet-centred position at `altitude_metres` above the surface, on the +Y axis. The frame
/// every function here uses: +Y is up at the origin of the world.
[[nodiscard]] Vec3 ground_position(const Atmosphere& atmosphere, f32 altitude_metres) noexcept;

// ================================================================================================
// AERIAL PERSPECTIVE
// ================================================================================================
//
// "Distance attenuation SHALL be produced by the atmosphere model... Ad-hoc distance fog with
// independently tuned parameters SHALL NOT be the engine's model of distance, though a stylised
// override SHALL remain available."
//
// `aerial_perspective()` is the model, and it takes the SAME `Atmosphere` the sky does — so distant
// terrain and the sky above it cannot be tuned apart. `StylisedDistance` is the override, and it is
// a declared struct rather than a second set of parameters that happens to be applied: a project
// that wants non-physical falloff says so in a field somebody can find.

/// What the atmosphere does to a surface at a distance: what survives, and what is added.
struct AerialPerspective {
    /// Multiplied into the surface's radiance, per channel.
    Vec3 transmittance{1.0F, 1.0F, 1.0F};
    /// Added after it, per channel. The scattered light between the eye and the surface.
    Vec3 in_scattering{0.0F, 0.0F, 0.0F};
};

/// A declared, explicit departure from the physical model. `strength` of 0 is the atmosphere's own
/// answer and 1 is entirely the stylised one; anything between is a blend, and the field's
/// existence is what makes the departure reviewable.
struct StylisedDistance {
    f32 strength = 0.0F;
    /// Metres at which the stylised term reaches half opacity.
    f32 half_distance = 2000.0F;
    Vec3 tint{0.6F, 0.7F, 0.85F};
};

/// The atmosphere's own distance attenuation between two planet-centred points.
[[nodiscard]] AerialPerspective aerial_perspective(const Atmosphere& atmosphere, Vec3 from, Vec3 to,
                                                   Vec3 sun_direction, u32 steps = 16) noexcept;

/// The same, blended towards a declared stylisation.
[[nodiscard]] AerialPerspective stylise(const AerialPerspective& physical,
                                        const StylisedDistance& style,
                                        f32 distance_metres) noexcept;

}  // namespace cy::rendering::sky
