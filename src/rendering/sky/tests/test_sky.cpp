// The atmosphere and the celestial model at unit cost: what the model refuses, that the sky's
// colour follows from the coefficients, that a different planet gets a different sky, and that the
// clock is a declared domain. The integrals — the GI sky term, the gradient fit, the tables — are
// `test_sky_light.cpp`, because they integrate the atmosphere thousands of times.

#include <cy/test/test.h>

#include <cy/core/math/math.h>
#include <cy/rendering/sky/atmosphere.h>
#include <cy/rendering/sky/celestial.h>

#include <cmath>

namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;
using cy::rendering::sky::advance_time_of_day;
using cy::rendering::sky::aerial_perspective;
using cy::rendering::sky::Atmosphere;
using cy::rendering::sky::blend_atmospheres;
using cy::rendering::sky::CelestialModel;
using cy::rendering::sky::earth_atmosphere;
using cy::rendering::sky::extinction_at;
using cy::rendering::sky::ground_position;
using cy::rendering::sky::light_travel_direction;
using cy::rendering::sky::mie_phase;
using cy::rendering::sky::rayleigh_phase;
using cy::rendering::sky::sky_radiance;
using cy::rendering::sky::solve_celestial;
using cy::rendering::sky::stylise;
using cy::rendering::sky::StylisedDistance;
using cy::rendering::sky::sun_illuminance;
using cy::rendering::sky::thin_dusty_atmosphere;
using cy::rendering::sky::TimeDomain;
using cy::rendering::sky::TimeOfDay;
using cy::rendering::sky::transmittance;
using cy::rendering::sky::validate_atmosphere;

const Vec3 kUp{0.0F, 1.0F, 0.0F};

}  // namespace

CY_TEST_CASE("atmosphere: an atmosphere that cannot describe one is refused") {
    CY_CHECK(validate_atmosphere(earth_atmosphere()));
    CY_CHECK(validate_atmosphere(thin_dusty_atmosphere()));

    Atmosphere inverted = earth_atmosphere();
    inverted.atmosphere_radius = inverted.planet_radius - 1.0F;
    CY_CHECK_FALSE(validate_atmosphere(inverted));

    Atmosphere impossible_mie = earth_atmosphere();
    // Extinction is scattering plus absorption, so it cannot be below the scattering it contains.
    // A model that allowed it would produce light out of haze.
    impossible_mie.mie_extinction = impossible_mie.mie_scattering * 0.5F;
    CY_CHECK_FALSE(validate_atmosphere(impossible_mie));

    Atmosphere impossible_phase = earth_atmosphere();
    impossible_phase.mie_anisotropy = 1.0F;
    CY_CHECK_FALSE(validate_atmosphere(impossible_phase));

    Atmosphere negative = earth_atmosphere();
    negative.rayleigh_scattering.y = -1.0e-6F;
    CY_CHECK_FALSE(validate_atmosphere(negative));
}

CY_TEST_CASE("atmosphere: the phase functions are normalised and behave the way they must") {
    // Rayleigh integrates to one over the sphere and is symmetric.
    f32 total = 0.0F;
    constexpr u32 kSteps = 512;
    for (u32 index = 0; index < kSteps; ++index) {
        const f32 cosine =
            -1.0F + (2.0F * (static_cast<f32>(index) + 0.5F) / static_cast<f32>(kSteps));
        total += rayleigh_phase(cosine) * (2.0F / static_cast<f32>(kSteps)) * 2.0F * cy::math::kPi;
    }
    CY_CHECK_NEAR(total, 1.0F, 0.01F);
    CY_CHECK_NEAR(rayleigh_phase(0.7F), rayleigh_phase(-0.7F), 1.0e-6F);

    // Mie is forward scattering, which is why the sky is bright around the sun.
    CY_CHECK_GT(mie_phase(1.0F, 0.8F), mie_phase(0.0F, 0.8F));
    CY_CHECK_GT(mie_phase(0.0F, 0.8F), mie_phase(-1.0F, 0.8F));
    // Isotropic at zero anisotropy: the same value in every direction.
    CY_CHECK_NEAR(mie_phase(1.0F, 0.0F), mie_phase(-1.0F, 0.0F), 1.0e-4F);
}

CY_TEST_CASE("atmosphere: extinction falls with altitude, and ozone sits in a layer") {
    const Atmosphere earth = earth_atmosphere();
    const Vec3 sea_level = extinction_at(earth, 0.0F);
    const Vec3 high = extinction_at(earth, 40000.0F);
    CY_CHECK_GT(sea_level.z, high.z);

    // Ozone is a TENT, not an exponential: it peaks in its layer and is absent above and below.
    // That shape is why a clear sky stays blue at twilight instead of going grey, and an
    // implementation that gave it a scale height would lose it.
    Atmosphere ozone_only = earth;
    ozone_only.rayleigh_scattering = Vec3{0.0F, 0.0F, 0.0F};
    ozone_only.mie_extinction = 0.0F;
    ozone_only.mie_scattering = 0.0F;
    CY_CHECK_GT(extinction_at(ozone_only, earth.ozone_center).y, extinction_at(ozone_only, 0.0F).y);
    CY_CHECK_NEAR(extinction_at(ozone_only, earth.ozone_center + earth.ozone_width).y, 0.0F,
                  1.0e-9F);
}

CY_TEST_CASE("atmosphere: transmittance is one at the top and zero through the planet") {
    const Atmosphere earth = earth_atmosphere();
    const Vec3 top{0.0F, earth.atmosphere_radius, 0.0F};
    const Vec3 through_the_top = transmittance(earth, top, kUp, 16);
    CY_CHECK_NEAR(through_the_top.x, 1.0F, 1.0e-3F);

    // Straight down from the surface: the ray meets the ground, and nothing reaches the top of the
    // atmosphere through a planet. Reporting zero rather than integrating on is what stops the sun
    // lighting a scene from below the horizon.
    const Vec3 ground = ground_position(earth, 0.0F);
    const Vec3 downwards = transmittance(earth, ground, Vec3{0.0F, -1.0F, 0.0F}, 16);
    CY_CHECK_EQ(downwards.x, 0.0F);

    // And straight up loses more blue than red, which is the same fact the sky's colour is.
    const Vec3 upwards = transmittance(earth, ground, kUp, 32);
    CY_CHECK_GT(upwards.x, upwards.z);
    CY_CHECK_LT(upwards.z, 1.0F);
}

CY_TEST_CASE("atmosphere: the sun reddens as it sets, because the path lengthens") {
    const Atmosphere earth = earth_atmosphere();
    const Vec3 ground = ground_position(earth, 0.0F);

    const Vec3 noon = sun_illuminance(earth, ground, kUp);
    const Vec3 low = sun_illuminance(earth, ground, normalize(Vec3{0.0F, 0.06F, 1.0F}));
    CY_CHECK_LT(low.x, noon.x);
    // The blue falls faster than the red, so the ratio moves: that IS sunset, and it comes out of
    // the coefficients rather than out of a colour ramp.
    CY_CHECK_GT(noon.z / noon.x, low.z / low.x);
    // Below the horizon: nothing.
    CY_CHECK_EQ(sun_illuminance(earth, ground, Vec3{0.0F, -0.5F, 0.5F}).x, 0.0F);
}

CY_TEST_CASE("atmosphere: a different planet gets a different sky, from its coefficients alone") {
    // The requirement's own scenario: "WHEN a project sets a dusty thin atmosphere THEN sky colour,
    // aerial perspective, and sunlight tint SHALL follow from the scattering parameters."
    const Atmosphere earth = earth_atmosphere();
    const Atmosphere dusty = thin_dusty_atmosphere();

    const Vec3 earth_zenith = sky_radiance(earth, ground_position(earth, 0.0F), kUp,
                                           normalize(Vec3{0.3F, 0.8F, 0.0F}), 16);
    const Vec3 dusty_zenith = sky_radiance(dusty, ground_position(dusty, 0.0F), kUp,
                                           normalize(Vec3{0.3F, 0.8F, 0.0F}), 16);

    // Earth's zenith is BLUE: several times as much short-wavelength radiance as long.
    CY_CHECK_GT(earth_zenith.z / cy::math::max(earth_zenith.x, 1.0e-6F), 2.5F);
    // The dusty planet's is nearly GREY, because its Rayleigh is a hundredth of Earth's and its Mie
    // — which is wavelength independent — is five times as much. It is not dimmer: a dusty sky is
    // bright. It is a different COLOUR, and the colour is what follows from the coefficients.
    CY_CHECK_LT(dusty_zenith.z / cy::math::max(dusty_zenith.x, 1.0e-6F), 1.6F);
}

CY_TEST_CASE("atmosphere: a composition change is a parameter transition, not a preset switch") {
    // "WHEN atmospheric composition changes during play THEN the sky SHALL follow, and the change
    // SHALL be a parameter transition rather than a preset switch."
    const Atmosphere earth = earth_atmosphere();
    const Atmosphere dusty = thin_dusty_atmosphere();
    const Atmosphere halfway = blend_atmospheres(earth, dusty, 0.5F);

    CY_CHECK(validate_atmosphere(halfway));
    CY_CHECK_NEAR(halfway.mie_anisotropy, (earth.mie_anisotropy + dusty.mie_anisotropy) * 0.5F,
                  1.0e-5F);
    CY_CHECK_NEAR(halfway.rayleigh_scattering.z,
                  (earth.rayleigh_scattering.z + dusty.rayleigh_scattering.z) * 0.5F, 1.0e-12F);
    // And the endpoints are the endpoints: a transition that did not reach either would be a
    // different failure and a subtler one.
    CY_CHECK_EQ(blend_atmospheres(earth, dusty, 0.0F).mie_scattering, earth.mie_scattering);
    CY_CHECK_EQ(blend_atmospheres(earth, dusty, 1.0F).mie_scattering, dusty.mie_scattering);
}

CY_TEST_CASE("atmosphere: aerial perspective comes from the same model as the sky") {
    // "Distance attenuation SHALL be produced by the atmosphere model... Ad-hoc distance fog with
    // independently tuned parameters SHALL NOT be the engine's model of distance." The enforcement
    // is the signature: this takes the same `Atmosphere` and there is no second parameter set.
    const Atmosphere earth = earth_atmosphere();
    const Vec3 eye = ground_position(earth, 2.0F);
    const Vec3 sun = normalize(Vec3{0.3F, 0.6F, 0.0F});

    const auto near_by = aerial_perspective(earth, eye, eye + Vec3{100.0F, 0.0F, 0.0F}, sun, 8);
    const auto far_away = aerial_perspective(earth, eye, eye + Vec3{20000.0F, 0.0F, 0.0F}, sun, 8);

    CY_CHECK_LT(far_away.transmittance.z, near_by.transmittance.z);
    CY_CHECK_GT(far_away.in_scattering.z, near_by.in_scattering.z);
    // And the distance haze is blue for the same reason the sky is, which is the consistency the
    // requirement is about.
    CY_CHECK_GT(far_away.in_scattering.z, far_away.in_scattering.x);

    // Zero distance changes nothing.
    const auto here = aerial_perspective(earth, eye, eye, sun, 8);
    CY_CHECK_EQ(here.transmittance.x, 1.0F);
    CY_CHECK_EQ(here.in_scattering.x, 0.0F);
}

CY_TEST_CASE(
    "atmosphere: a stylised distance override is declared and reaches the physical model") {
    const Atmosphere earth = earth_atmosphere();
    const Vec3 eye = ground_position(earth, 2.0F);
    const auto physical = aerial_perspective(earth, eye, eye + Vec3{5000.0F, 0.0F, 0.0F},
                                             normalize(Vec3{0.3F, 0.6F, 0.0F}), 8);

    StylisedDistance none;
    CY_CHECK_EQ(stylise(physical, none, 5000.0F).transmittance.x, physical.transmittance.x);

    StylisedDistance heavy;
    heavy.strength = 1.0F;
    heavy.half_distance = 1000.0F;
    const auto stylised = stylise(physical, heavy, 1000.0F);
    // One half distance: half of the surface survives, by the definition the field's name gives.
    CY_CHECK_NEAR(stylised.transmittance.x, 0.5F, 1.0e-4F);
}

CY_TEST_CASE("celestial: the sun rises, crosses and sets on the declared clock") {
    CelestialModel model;
    model.latitude_degrees = 40.0F;
    TimeOfDay time;
    time.day_of_year = 172.0F;  // the June solstice, so the sun is high at this latitude

    time.fraction = 0.5F;  // local noon
    const auto noon = solve_celestial(model, time);
    CY_CHECK(noon.sun.above_horizon);
    CY_CHECK_GT(noon.sun.direction.y, 0.7F);
    CY_CHECK_EQ(noon.star_visibility, 0.0F);

    time.fraction = 0.0F;  // midnight
    const auto midnight = solve_celestial(model, time);
    CY_CHECK_FALSE(midnight.sun.above_horizon);
    CY_CHECK_LT(midnight.sun.direction.y, 0.0F);
    CY_CHECK_EQ(midnight.star_visibility, 1.0F);

    // And the light the sun becomes travels the other way, which is the one sign error that lights
    // a scene from exactly the wrong side and looks plausible until a shadow is compared.
    CY_CHECK_NEAR(dot(light_travel_direction(noon.sun), noon.sun.direction), -1.0F, 1.0e-4F);
}

CY_TEST_CASE("celestial: the axial tilt is why there are seasons, and zero tilt removes them") {
    CelestialModel model;
    model.latitude_degrees = 55.0F;
    TimeOfDay time;
    time.fraction = 0.5F;

    time.day_of_year = 172.0F;  // June
    const f32 summer = solve_celestial(model, time).sun.direction.y;
    time.day_of_year = 355.0F;  // December
    const f32 winter = solve_celestial(model, time).sun.direction.y;
    CY_CHECK_GT(summer, winter);

    model.axial_tilt_degrees = 0.0F;
    time.day_of_year = 172.0F;
    const f32 flat_summer = solve_celestial(model, time).sun.direction.y;
    time.day_of_year = 355.0F;
    const f32 flat_winter = solve_celestial(model, time).sun.direction.y;
    CY_CHECK_NEAR(flat_summer, flat_winter, 1.0e-4F);
}

CY_TEST_CASE("celestial: the time domain is declared, and a short day follows it") {
    // "WHEN a project maps one real minute to twenty game minutes THEN the celestial model SHALL
    // follow that mapping." A twenty-minute day is `seconds_per_day = 1200`.
    TimeOfDay time;
    time.domain = TimeDomain::Gameplay;
    time.seconds_per_day = 1200.0F;
    time.fraction = 0.0F;
    time.day_of_year = 10.0F;

    advance_time_of_day(time, 600.0F);  // half a day
    CY_CHECK_NEAR(time.fraction, 0.5F, 1.0e-5F);
    CY_CHECK_EQ(time.day_of_year, 10.0F);

    advance_time_of_day(time, 600.0F);  // the rest of it
    CY_CHECK_NEAR(time.fraction, 0.0F, 1.0e-4F);
    CY_CHECK_EQ(time.day_of_year, 11.0F);

    // Paused: the sky stops without any call site having to remember not to advance it.
    time.paused = true;
    advance_time_of_day(time, 5000.0F);
    CY_CHECK_EQ(time.day_of_year, 11.0F);

    // A custom domain advances only by assignment, which is what a scripted sky needs.
    time.paused = false;
    time.domain = TimeDomain::Custom;
    advance_time_of_day(time, 5000.0F);
    CY_CHECK_EQ(time.day_of_year, 11.0F);
    // The domain's own spelling reaches a diagnostic, which is what makes "declared" visible.
    CY_CHECK(cy::rendering::sky::time_domain_name(TimeDomain::Custom) != nullptr);
}

CY_TEST_CASE("celestial: the model is bypassable, and bypassing it costs nothing") {
    // "WHEN a project places the sun directly for art direction THEN the celestial model SHALL be
    // bypassable without losing atmosphere or sky." The proof is that the sky takes a direction and
    // never a model: an authored sun goes straight into `sky_radiance`.
    cy::rendering::sky::CelestialBody authored;
    authored.direction = normalize(Vec3{0.4F, 0.35F, -0.85F});
    authored.above_horizon = true;

    const Atmosphere earth = earth_atmosphere();
    const Vec3 radiance =
        sky_radiance(earth, ground_position(earth, 0.0F), kUp, authored.direction, 16);
    CY_CHECK_GT(radiance.z, 0.0F);
    CY_CHECK_GT(sun_illuminance(earth, ground_position(earth, 0.0F), authored.direction).x, 0.0F);
}

CY_TEST_CASE("celestial: the moon has a phase, and it is not always the same one") {
    CelestialModel model;
    TimeOfDay time;
    time.fraction = 0.0F;

    f32 lowest = 2.0F;
    f32 highest = -1.0F;
    for (u32 day = 0; day < 30; ++day) {
        time.day_of_year = static_cast<f32>(day);
        const auto state = solve_celestial(model, time);
        lowest = cy::math::min(lowest, state.moon.illuminated_fraction);
        highest = cy::math::max(highest, state.moon.illuminated_fraction);
        CY_CHECK_NEAR(length(state.moon.direction), 1.0F, 1.0e-3F);
    }
    CY_CHECK_LT(lowest, 0.1F);
    CY_CHECK_GT(highest, 0.9F);
}
