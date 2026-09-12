// The environment sample: the three qualities, the batch that allocates nothing, and the
// downgrade that is honest rather than silent. M10 task 3.1.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`:
// `EnvironmentSampler::sample()`'s downgrade was removed, so a `HighFrequency` request from an
// authoritative reader returned a sample whose wind carried the turbulence residual. "an
// authoritative reader asking for high frequency is downgraded, not given presentation state" went
// red on the reported quality and on the non-zero turbulence. The downgrade was restored.

#include <cy/test/test.h>

#include <cy/weather/sample.h>
#include <cy/weather/system.h>

#include <cmath>

#include "fixtures.h"

namespace test = cy::weather::test;
using cy::weather::ClimateMap;
using cy::weather::EnvironmentSample;
using cy::weather::RefinementSource;
using cy::weather::SampleQuality;
using cy::weather::WeatherScale;
using cy::weather::WeatherSystem;

namespace {

[[nodiscard]] ClimateMap& uniform_climate() {
    static ClimateMap map(test::allocator());
    static const bool ready = [] {
        return map.set_uniform(test::temperate_climate()).has_value();
    }();
    (void)ready;
    return map;
}

}  // namespace

CY_TEST_CASE("the sample carries every quantity the specification lists") {
    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(system.advance(test::at_tick(1), 600.0).has_value());

    const EnvironmentSample sample =
        system.sample(cy::world::WorldVec3d{4'000.0, 2.0, 0.0}, SampleQuality::Gameplay,
                      cy::determinism::SimulationClass::Authoritative);
    // temperature, humidity, pressure, wind, precipitation rate by type, cloud coverage,
    // visibility, and the derived values.
    CY_CHECK_GT(sample.temperature_celsius, -60.0F);
    CY_CHECK_GE(sample.humidity, 0.0F);
    CY_CHECK_LE(sample.humidity, 1.0F);
    CY_CHECK_GT(sample.pressure_hpa, 800.0F);
    CY_CHECK_GT(sample.wind.speed(), 0.0F);
    CY_CHECK_GE(sample.precipitation_mm_per_hour, 0.0F);
    CY_CHECK_GE(sample.cloud_coverage, 0.0F);
    CY_CHECK_LE(sample.cloud_coverage, 1.0F);
    CY_CHECK_GT(sample.visibility_metres, 0.0F);
    CY_CHECK_GE(sample.wetness_potential, 0.0F);
    CY_CHECK_LE(sample.wetness_potential, 1.0F);
    // And the resolution indicator, which is what makes a fallback honest rather than silent.
    CY_CHECK(sample.quality == SampleQuality::Gameplay);
    CY_CHECK_GT(sample.cell_metres, 0.0F);
}

CY_TEST_CASE("macro and gameplay never disagree about what macro reports") {
    // sample.h's claim: macro is not an approximation with its own error, it is the same weather
    // cell read with fewer terms added on top. An agent reasoning at macro quality and a simulation
    // acting at gameplay quality therefore cannot reach contradictory conclusions about which
    // valley is raining.
    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(test::config(), uniform_climate()).has_value());
    system.set_terrain(test::ridge());
    CY_REQUIRE(system.advance(test::at_tick(1), 3'600.0).has_value());

    for (cy::u32 index = 0; index < 200; ++index) {
        const cy::world::WorldVec3d at{static_cast<cy::f64>(index) * 431.0, 2.0, 97.0};
        const EnvironmentSample macro = system.sample(
            at, SampleQuality::Macro, cy::determinism::SimulationClass::Authoritative);
        const EnvironmentSample gameplay = system.sample(
            at, SampleQuality::Gameplay, cy::determinism::SimulationClass::Authoritative);
        CY_CHECK_EQ(macro.temperature_celsius, gameplay.temperature_celsius);
        CY_CHECK_EQ(macro.humidity, gameplay.humidity);
        CY_CHECK_EQ(macro.precipitation_mm_per_hour, gameplay.precipitation_mm_per_hour);
        CY_CHECK(macro.precipitation_type == gameplay.precipitation_type);
        CY_CHECK_EQ(macro.visibility_metres, gameplay.visibility_metres);
    }
}

CY_TEST_CASE(
    "an authoritative reader asking for high frequency is downgraded, not given "
    "presentation state") {
    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(system.advance(test::at_tick(1), 600.0).has_value());

    const cy::world::WorldVec3d at{1'000.0, 3.0, 1'000.0};
    const EnvironmentSample refused = system.sample(
        at, SampleQuality::HighFrequency, cy::determinism::SimulationClass::Authoritative);
    // The request is answered, and the answer says what it actually is. Refusing would make a legal
    // question an error; answering silently with presentation state would be the crossing itself.
    CY_CHECK(refused.quality == SampleQuality::Gameplay);
    CY_CHECK_EQ(refused.wind.turbulence.x, 0.0F);
    CY_CHECK_EQ(refused.wind.turbulence.z, 0.0F);

    const EnvironmentSample allowed = system.sample(at, SampleQuality::HighFrequency,
                                                    cy::determinism::SimulationClass::Presentation);
    CY_CHECK(allowed.quality == SampleQuality::HighFrequency);
    // The authoritative halves still agree exactly.
    CY_CHECK_EQ(refused.wind.authoritative().x, allowed.wind.authoritative().x);
    CY_CHECK_EQ(refused.wind.authoritative().z, allowed.wind.authoritative().z);
}

CY_TEST_CASE("high frequency reaches a refined cell where one exists and falls back where none") {
    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(test::config(), uniform_climate()).has_value());
    RefinementSource source;
    source.x = 0.0;
    source.z = 0.0;
    source.radius_metres = 2'000.0F;
    CY_REQUIRE(system.cells()
                   .set_refinement_sources(cy::Span<const RefinementSource>(&source, 1))
                   .has_value());
    CY_REQUIRE(system.advance(test::at_tick(1), 600.0).has_value());

    const EnvironmentSample near =
        system.sample(cy::world::WorldVec3d{0.0, 1.0, 0.0}, SampleQuality::HighFrequency,
                      cy::determinism::SimulationClass::Presentation);
    CY_CHECK(near.scale == WeatherScale::Local);

    const EnvironmentSample away =
        system.sample(cy::world::WorldVec3d{70'000.0, 1.0, 0.0}, SampleQuality::HighFrequency,
                      cy::determinism::SimulationClass::Presentation);
    // "SHALL return the coarsest resident value with a resolution indicator, and SHALL NOT block."
    CY_CHECK(away.scale == WeatherScale::Regional);
    CY_CHECK_GT(away.cell_metres, near.cell_metres);
}

CY_TEST_CASE("a batch of four thousand samples matches the same samples taken one at a time") {
    // "Sampling SHALL be batchable and SHALL NOT allocate." The allocation half is structural — the
    // batch writes into the caller's span — and this is the agreement half.
    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(test::config(), uniform_climate()).has_value());
    system.set_terrain(test::ridge());
    CY_REQUIRE(system.advance(test::at_tick(1), 1'800.0).has_value());

    cy::Array<cy::world::WorldVec3d> positions(test::allocator());
    cy::Array<EnvironmentSample> out(test::allocator());
    CY_REQUIRE(positions.resize(4'000).has_value());
    CY_REQUIRE(out.resize(4'000).has_value());
    for (cy::usize index = 0; index < positions.size(); ++index) {
        positions[index] = cy::world::WorldVec3d{static_cast<cy::f64>(index) * 17.0, 2.0,
                                                 static_cast<cy::f64>(index % 53) * 91.0};
    }
    CY_REQUIRE(system.sampler()
                   .sample_many(positions.span(), SampleQuality::Macro,
                                cy::determinism::SimulationClass::Authoritative, out.span())
                   .has_value());
    for (cy::usize index = 0; index < positions.size(); index += 211) {
        const EnvironmentSample one =
            system.sample(positions[index], SampleQuality::Macro,
                          cy::determinism::SimulationClass::Authoritative);
        CY_CHECK_EQ(one.temperature_celsius, out[index].temperature_celsius);
        CY_CHECK_EQ(one.precipitation_mm_per_hour, out[index].precipitation_mm_per_hour);
    }
}

CY_TEST_CASE("wetness potential is the accumulation's target, and it freezes below zero") {
    cy::weather::WeatherState raining;
    raining.precipitation_mm_per_hour = 8.0F;
    raining.temperature_celsius = 12.0F;
    cy::weather::WeatherState misty;
    misty.humidity = 0.95F;
    misty.temperature_celsius = 12.0F;
    cy::weather::WeatherState frozen = raining;
    frozen.temperature_celsius = -6.0F;

    CY_CHECK_NEAR(cy::weather::wetness_potential_of(raining), 1.0F, 0.01F);
    CY_CHECK_GT(cy::weather::wetness_potential_of(misty), 0.2F);
    CY_CHECK_LT(cy::weather::wetness_potential_of(misty), 0.6F);
    // Below freezing a surface ices rather than wets, and what falls accumulates as snow instead —
    // which is `accumulation.h`'s other field.
    CY_CHECK_LT(cy::weather::wetness_potential_of(frozen),
                cy::weather::wetness_potential_of(raining) * 0.3F);
}
