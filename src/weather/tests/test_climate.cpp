// Climate: the slow layer, the biome potential it feeds, and the requirement that it is not
// consulted to answer a question about the current state. M10 task 3.1.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`: `WindComposer::
// compose()`'s prevailing term was pointed back at a `ClimateMap` instead of at
// `WeatherCells::climate_at()`. "sampling the environment does not consult the climate" went red
// immediately, reporting 10 201 evaluations where it required zero. The composer was restored.

#include <cy/test/test.h>

#include <cy/weather/climate.h>
#include <cy/weather/system.h>

#include "fixtures.h"

namespace test = cy::weather::test;
using cy::weather::BiomePotential;
using cy::weather::ClimateMap;
using cy::weather::ClimateModel;
using cy::weather::ClimateSample;
using cy::weather::ClimateSourceKind;
using cy::weather::ClimateTerrain;
using cy::weather::WeatherSystem;

namespace {

cy::f64 sea_level(void*, cy::f64, cy::f64) noexcept {
    return 0.0;
}
cy::f64 alpine(void*, cy::f64, cy::f64) noexcept {
    return 2'500.0;
}
cy::f64 far_inland(void*, cy::f64, cy::f64) noexcept {
    return 400'000.0;
}
cy::f64 coastal(void*, cy::f64, cy::f64) noexcept {
    return 0.0;
}

}  // namespace

CY_TEST_CASE("a uniform climate answers the same everywhere and costs one evaluation a sample") {
    ClimateMap map(test::allocator());
    CY_REQUIRE(map.set_uniform(test::temperate_climate()).has_value());
    CY_CHECK(map.kind() == ClimateSourceKind::Uniform);

    const ClimateSample here = map.sample(0.0, 0.0);
    const ClimateSample far = map.sample(4'000'000.0, -900'000.0);
    CY_CHECK_EQ(here.mean_temperature_celsius, far.mean_temperature_celsius);
    CY_CHECK_EQ(here.rainfall_potential_mm, far.rainfall_potential_mm);
}

CY_TEST_CASE("the derived model is colder up a mountain and drier away from the sea") {
    ClimateModel model;
    ClimateTerrain flat;
    flat.elevation_at = sea_level;
    flat.ocean_distance_at = coastal;
    ClimateTerrain high;
    high.elevation_at = alpine;
    high.ocean_distance_at = coastal;
    ClimateTerrain dry;
    dry.elevation_at = sea_level;
    dry.ocean_distance_at = far_inland;

    const ClimateSample at_sea = cy::weather::derive_climate(model, flat, 0.0, 0.0);
    const ClimateSample up = cy::weather::derive_climate(model, high, 0.0, 0.0);
    const ClimateSample inland = cy::weather::derive_climate(model, dry, 0.0, 0.0);

    // 2 500 m at 6.5 C per kilometre is sixteen degrees. The lapse rate is a DECLARED model number,
    // so this is an arithmetic check on the model rather than a tuning check on the world.
    CY_CHECK_NEAR(at_sea.mean_temperature_celsius - up.mean_temperature_celsius, 16.25F, 0.01F);
    CY_CHECK_GT(at_sea.ocean_influence, 0.9F);
    CY_CHECK_LT(inland.ocean_influence, 0.05F);
    // The sea moderates the swing and raises the humidity, which are the same physical fact seen in
    // the two members a designer reads.
    CY_CHECK_LT(at_sea.temperature_range_celsius, inland.temperature_range_celsius);
    CY_CHECK_GT(at_sea.humidity, inland.humidity);
}

CY_TEST_CASE("biome potential follows Whittaker's axes, not the rainfall alone") {
    ClimateSample hot_wet = test::temperate_climate();
    hot_wet.mean_temperature_celsius = 26.0F;
    hot_wet.rainfall_potential_mm = 3'000.0F;
    ClimateSample hot_dry = hot_wet;
    hot_dry.rainfall_potential_mm = 90.0F;
    ClimateSample cold_wet = hot_wet;
    cold_wet.mean_temperature_celsius = -12.0F;

    const BiomePotential wet = cy::weather::climate_biome_potential(hot_wet);
    const BiomePotential desert = cy::weather::climate_biome_potential(hot_dry);
    const BiomePotential tundra = cy::weather::climate_biome_potential(cold_wet);

    CY_CHECK_EQ(wet.biome, cy::weather::biomes::kRainforest);
    CY_CHECK_EQ(desert.biome, cy::weather::biomes::kDesert);
    CY_CHECK_EQ(tundra.biome, cy::weather::biomes::kTundra);

    // The same rainfall means different things at different temperatures, which is the whole point
    // of the moisture index: 900 mm is wet where it is cold and merely adequate where it is hot.
    ClimateSample cool = test::temperate_climate();
    cool.mean_temperature_celsius = 4.0F;
    ClimateSample warm = cool;
    warm.mean_temperature_celsius = 28.0F;
    CY_CHECK_GT(cy::weather::climate_biome_potential(cool).moisture,
                cy::weather::climate_biome_potential(warm).moisture);
}

CY_TEST_CASE("a derived grid is evaluated once, at build time") {
    ClimateMap map(test::allocator());
    ClimateModel model;
    ClimateTerrain terrain;
    terrain.elevation_at = sea_level;
    terrain.ocean_distance_at = coastal;
    CY_REQUIRE(map.derive(model, terrain, 0.0, 0.0, 20'000.0F, 8, 8).has_value());
    // Sixty-four cells, sixty-four evaluations. That is the affordability claim climate.h makes,
    // stated as a number rather than as an adjective.
    CY_CHECK_EQ(map.evaluations(), 64u);
}

CY_TEST_CASE("sampling the environment does not consult the climate") {
    // THE REQUIREMENT: "climate SHALL NOT be recomputed to answer a question about the current
    // state." The counter is the enforcement — see climate.h's header note — and this is where it
    // is required not to move.
    ClimateMap map(test::allocator());
    CY_REQUIRE(map.set_uniform(test::temperate_climate()).has_value());

    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(test::config(), map).has_value());
    CY_REQUIRE(system.advance(test::at_tick(1), 1.0).has_value());

    // THE INSTRUMENT FIRST. `evaluations() == 0` below would pass for free if the counter never
    // moved at all — which is the trap this project has been caught by before — so the counter is
    // shown to be live against a known number before it is asked for a zero.
    map.reset_evaluations();
    for (cy::u32 index = 0; index < 100; ++index) {
        (void)map.sample(static_cast<cy::f64>(index) * 3.0, 0.0);
    }
    CY_CHECK_EQ(map.evaluations(), 100u);

    map.reset_evaluations();
    for (cy::u32 index = 0; index < 2'000; ++index) {
        const auto x = static_cast<cy::f64>(index) * 13.0;
        const cy::weather::EnvironmentSample sample = system.sample(
            cy::world::WorldVec3d{x, 0.0, x * 0.5}, cy::weather::SampleQuality::Gameplay,
            cy::determinism::SimulationClass::Authoritative);
        CY_CHECK(sample.visibility_metres > 0.0F);
    }
    CY_CHECK_EQ(map.evaluations(), 0u);

    // And the wind — which the specification lists "prevailing wind FROM CLIMATE" as a term of —
    // does not consult it either: it reads the climate the cell was SEEDED with.
    for (cy::u32 index = 0; index < 300; ++index) {
        const auto x = static_cast<cy::f64>(index) * 37.0;
        const cy::weather::WindSample wind = system.wind().sample(
            cy::world::WorldVec3d{x, 2.0, 0.0}, cy::determinism::SimulationClass::Presentation,
            test::at_tick(1), 1.0);
        CY_CHECK(wind.speed() >= 0.0F);
    }
    CY_CHECK_EQ(map.evaluations(), 0u);
}

CY_TEST_CASE("an authored grid saturates rather than defaulting outside itself") {
    ClimateMap map(test::allocator());
    ClimateSample cells[4];
    for (cy::u32 index = 0; index < 4; ++index) {
        cells[index] = test::temperate_climate();
        cells[index].mean_temperature_celsius = 10.0F + static_cast<cy::f32>(index);
    }
    CY_REQUIRE(map.set_grid(0.0, 0.0, 1'000.0F, 2, 2, cy::Span<const ClimateSample>(cells, 4))
                   .has_value());

    // A million metres west of the authored rectangle is still cell (0,0)'s climate, not a default
    // nobody painted. A world's climate does not stop at the edge of the map somebody made.
    const ClimateSample outside = map.sample(-1'000'000.0, -1'000'000.0);
    CY_CHECK_NEAR(outside.mean_temperature_celsius, 10.0F, 0.001F);
    const ClimateSample beyond = map.sample(1'000'000.0, 1'000'000.0);
    CY_CHECK_NEAR(beyond.mean_temperature_celsius, 13.0F, 0.001F);
}
