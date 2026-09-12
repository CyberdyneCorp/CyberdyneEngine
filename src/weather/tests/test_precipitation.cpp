// Precipitation: the types, the occlusion that answers "am I sheltered" without a ray per drop, and
// the tiers that make heavy rain cost what light rain costs. M10 task 3.2.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`: the particle cap in
// `plan_precipitation()` was changed from `min(max_particles, wanted)` to `wanted`. "rain scales in
// tiers rather than in particles" went red immediately, reporting 409 600 particles at a hundred
// times the rate where it required at most 4 096. The cap was restored.

#include <cy/test/test.h>

#include <cy/weather/precipitation.h>

#include "fixtures.h"

namespace test = cy::weather::test;
using cy::weather::InteractionRates;
using cy::weather::PrecipitationLevers;
using cy::weather::PrecipitationPlan;
using cy::weather::PrecipitationTier;
using cy::weather::PrecipitationType;
using cy::weather::ShelterSample;
using cy::weather::SkyOcclusion;
using cy::weather::WeatherState;

namespace {

[[nodiscard]] WeatherState raining(cy::f32 rate) noexcept {
    WeatherState state;
    state.precipitation_mm_per_hour = rate;
    state.precipitation_type = PrecipitationType::Rain;
    state.temperature_celsius = 11.0F;
    return state;
}

}  // namespace

CY_TEST_CASE("a type's properties are data, and snow is the one that lies on the ground") {
    CY_CHECK(cy::weather::precipitation_properties(PrecipitationType::Rain).frozen == false);
    CY_CHECK(cy::weather::precipitation_properties(PrecipitationType::Snow).frozen);
    CY_CHECK_GT(cy::weather::precipitation_properties(PrecipitationType::Snow).depth_yield, 0.0F);
    CY_CHECK_EQ(cy::weather::precipitation_properties(PrecipitationType::Rain).depth_yield, 0.0F);
    // Hail falls fast and rain falls slowly, which is the only reason this module knows a fall
    // speed at all: it is what the presentation tier needs to slant a particle.
    CY_CHECK_GT(cy::weather::precipitation_properties(PrecipitationType::Hail).fall_speed,
                cy::weather::precipitation_properties(PrecipitationType::Rain).fall_speed);

    // Temperature decides rain against snow; a storm that declares ash or dust owns its type.
    CY_CHECK(cy::weather::precipitation_type_for(10.0F, PrecipitationType::Rain) ==
             PrecipitationType::Rain);
    CY_CHECK(cy::weather::precipitation_type_for(-3.0F, PrecipitationType::Rain) ==
             PrecipitationType::Snow);
    CY_CHECK(cy::weather::precipitation_type_for(-3.0F, PrecipitationType::Ash) ==
             PrecipitationType::Ash);
}

CY_TEST_CASE("indoors is dry, and the answer costs one lookup whatever the rain does") {
    // "Whether a position is sheltered SHALL be answered by a PRECIPITATION OCCLUSION
    // REPRESENTATION — a coarse sky-visibility structure derived from scene geometry — not by a ray
    // per drop."
    SkyOcclusion occlusion(test::allocator());
    CY_REQUIRE(occlusion.configure(2.0F).has_value());
    CY_REQUIRE(occlusion.add_cover(0.0, 0.0, 10.0, 10.0, 4.0F, 1.0F).has_value());

    const ShelterSample inside = occlusion.shelter_at(cy::world::WorldVec3d{5.0, 1.6, 5.0});
    CY_CHECK(inside.sheltered);
    CY_CHECK_NEAR(inside.sky_visibility, 0.0F, 0.001F);
    CY_CHECK_NEAR(inside.cover_height_metres, 2.4F, 0.001F);

    // On the roof, not under it.
    const ShelterSample above = occlusion.shelter_at(cy::world::WorldVec3d{5.0, 9.0, 5.0});
    CY_CHECK_FALSE(above.sheltered);
    CY_CHECK_NEAR(above.sky_visibility, 1.0F, 0.001F);

    // Outside the covered rectangle.
    const ShelterSample outside = occlusion.shelter_at(cy::world::WorldVec3d{50.0, 1.6, 5.0});
    CY_CHECK_FALSE(outside.sheltered);

    // A CANOPY is not a roof: it hides part of the sky and the rest gets through.
    CY_REQUIRE(occlusion.add_cover(100.0, 0.0, 110.0, 10.0, 8.0F, 0.6F).has_value());
    const ShelterSample under_trees = occlusion.shelter_at(cy::world::WorldVec3d{105.0, 1.6, 5.0});
    CY_CHECK(under_trees.sheltered);
    CY_CHECK_NEAR(under_trees.sky_visibility, 0.4F, 0.001F);

    // THE COST. A hundred shelter queries cost a hundred lookups, whatever the rain rate is —
    // because there is no per-drop anything in the module to scale with it.
    occlusion.reset_queries();
    for (cy::u32 index = 0; index < 100; ++index) {
        (void)occlusion.shelter_at(cy::world::WorldVec3d{static_cast<cy::f64>(index), 1.0, 3.0});
    }
    CY_CHECK_EQ(occlusion.queries(), 100u);
}

CY_TEST_CASE("two covers over one cell take the lowest and compose their occlusions") {
    SkyOcclusion occlusion(test::allocator());
    CY_REQUIRE(occlusion.configure(4.0F).has_value());
    CY_REQUIRE(occlusion.add_cover(0.0, 0.0, 8.0, 8.0, 12.0F, 0.5F).has_value());
    CY_REQUIRE(occlusion.add_cover(0.0, 0.0, 8.0, 8.0, 5.0F, 0.5F).has_value());

    const ShelterSample under = occlusion.shelter_at(cy::world::WorldVec3d{2.0, 1.0, 2.0});
    // A position under a bridge under a roof is sheltered by the BRIDGE — the lowest thing over it.
    CY_CHECK_NEAR(under.cover_height_metres, 4.0F, 0.001F);
    // And two independent screens each letting half through let a quarter through.
    CY_CHECK_NEAR(under.sky_visibility, 0.25F, 0.001F);
}

CY_TEST_CASE("rain scales in tiers rather than in particles") {
    // "WHEN heavy rain falls across a large view THEN it SHALL be rendered in tiers rather than as
    // millions of simulated drops."
    PrecipitationLevers levers;
    levers.max_particles = 4'096;

    const PrecipitationPlan light =
        cy::weather::plan_precipitation(raining(1.0F), cy::Vec2{3.0F, 0.0F}, levers);
    const PrecipitationPlan heavy =
        cy::weather::plan_precipitation(raining(100.0F), cy::Vec2{3.0F, 0.0F}, levers);

    CY_CHECK_GT(light.particles[static_cast<cy::u32>(PrecipitationTier::Particles)], 0u);
    CY_CHECK_LE(heavy.particles[static_cast<cy::u32>(PrecipitationTier::Particles)],
                levers.max_particles);
    // A hundred times the rate is NOT a hundred times the particles; the extra is carried by the
    // approximation tier's density, which is the tiering doing its job.
    CY_CHECK_GT(heavy.approximation_density, light.approximation_density);
    // The other two tiers draw no particles at all. That is what "tiered" means here, and carrying
    // the zeroes explicitly is so a diagnostic can show them rather than leave them implied.
    CY_CHECK_EQ(heavy.particles[static_cast<cy::u32>(PrecipitationTier::Approximation)], 0u);
    CY_CHECK_EQ(heavy.particles[static_cast<cy::u32>(PrecipitationTier::FieldOnly)], 0u);
    CY_CHECK_NEAR(heavy.tier_end_metres[0], levers.particle_distance_metres, 0.001F);
    CY_CHECK_NEAR(heavy.tier_start_metres[2], levers.approximation_distance_metres, 0.001F);

    // No rain, no plan.
    WeatherState dry;
    const PrecipitationPlan none =
        cy::weather::plan_precipitation(dry, cy::Vec2{3.0F, 0.0F}, levers);
    CY_CHECK_EQ(none.particles[0], 0u);
}

CY_TEST_CASE("the slant is one number every tier shares, and it follows the wind") {
    PrecipitationLevers levers;
    const PrecipitationPlan calm =
        cy::weather::plan_precipitation(raining(5.0F), cy::Vec2{0.0F, 0.0F}, levers);
    const PrecipitationPlan blown =
        cy::weather::plan_precipitation(raining(5.0F), cy::Vec2{14.0F, 0.0F}, levers);
    CY_CHECK_NEAR(calm.slant.x, 0.0F, 0.001F);
    CY_CHECK_GT(blown.slant.x, 1.0F);

    // Snow falls slowly, so the same wind carries it far further sideways. A presentation system
    // that slanted both alike would make a blizzard look like rain.
    WeatherState snow = raining(5.0F);
    snow.precipitation_type = PrecipitationType::Snow;
    snow.temperature_celsius = -4.0F;
    const PrecipitationPlan snowing =
        cy::weather::plan_precipitation(snow, cy::Vec2{14.0F, 0.0F}, levers);
    CY_CHECK_GT(snowing.slant.x, blown.slant.x * 3.0F);
}

CY_TEST_CASE("the presentation levers move the plan and nothing else") {
    // "Presentation budgets SHALL be levers of the renderer budget arbiter, and reducing them SHALL
    // NOT alter authoritative weather state." Here at the function level: the plan is computed FROM
    // the state, and the state it was computed from is a const reference nothing writes through.
    PrecipitationLevers full;
    PrecipitationLevers reduced;
    reduced.max_particles = 128;
    reduced.density_scale = 0.25F;

    const WeatherState state = raining(20.0F);
    const PrecipitationPlan rich =
        cy::weather::plan_precipitation(state, cy::Vec2{5.0F, 0.0F}, full);
    const PrecipitationPlan poor =
        cy::weather::plan_precipitation(state, cy::Vec2{5.0F, 0.0F}, reduced);

    CY_CHECK_GT(rich.particles[0], poor.particles[0]);
    // The RATE and the TYPE are identical, and they are what gameplay reads.
    CY_CHECK_EQ(rich.rate_mm_per_hour, poor.rate_mm_per_hour);
    CY_CHECK(rich.type == poor.type);
    CY_CHECK_EQ(rich.approximation_density, poor.approximation_density);
}

CY_TEST_CASE("splashes and drips come from the occlusion and the wetness, not from collisions") {
    // "Interaction effects — splashes, drips, surface impacts — SHALL be driven by that occlusion
    // and by the wetness field rather than by particle collision events." The signature is the
    // enforcement: there is no collision event in it.
    const WeatherState state = raining(6.0F);
    ShelterSample open;
    ShelterSample covered;
    covered.sheltered = true;
    covered.sky_visibility = 0.1F;
    covered.cover_height_metres = 3.0F;

    const InteractionRates outside = cy::weather::interaction_rates(state, open, 0.8F);
    const InteractionRates inside = cy::weather::interaction_rates(state, covered, 0.8F);

    CY_CHECK_GT(outside.splash_rate, inside.splash_rate * 4.0F);
    CY_CHECK_EQ(outside.drip_rate, 0.0F);
    CY_CHECK_GT(inside.drip_rate, 0.0F);
    // A dry dusty surface does not splash as hard as a wet one.
    CY_CHECK_GT(cy::weather::interaction_rates(state, open, 1.0F).splash_rate,
                cy::weather::interaction_rates(state, open, 0.0F).splash_rate);
}
