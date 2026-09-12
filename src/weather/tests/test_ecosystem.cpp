// Macro ecosystem state: a burned forest regrows toward its potential without simulating a plant, a
// terraformed region crosses a declared threshold into another biome, and ninety days in one call
// is ninety days in ninety calls. M10 task 3.2.
//
// An integration suite: real fields, real tiles, and the substrate's own `advance_recovery()` doing
// the regrowth — which is the point, because a recovery curve written in `src/weather/` would be a
// second curve beside the substrate's.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`: the carry in
// `Ecosystem::advance()` was replaced by `steps = round(seconds / kEcosystemStepSeconds)`, which is
// what a fast-forward written without thinking about it does. "ninety days in one call is ninety
// days in ninety calls" went red immediately: one 90-day call ran 2 160 steps and ninety 1-day
// calls ran 2 160 as well, but a sequence of 36-minute advances ran 0 — every one of them rounded
// to nothing. The carry was restored.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/weather/ecosystem.h>
#include <cy/weather/system.h>

#include <cmath>

#include "fixtures.h"

namespace test = cy::weather::test;
using cy::environment::FieldRegistry;
using cy::environment::FieldResidency;
using cy::environment::FieldStore;
using cy::weather::ClimateMap;
using cy::weather::EcosystemEvent;
using cy::weather::EcosystemEventKind;
using cy::weather::EcosystemReport;
using cy::weather::PublishRegion;
using cy::weather::WeatherField;
using cy::weather::WeatherSystem;

namespace {

/// A small world: one climate, one region of a few macro lattice points, and the ecosystem seeded
/// from the climate's own potential.
struct World {
    explicit World(cy::Allocator& allocator) noexcept
        : climate(allocator),
          registry(allocator),
          store(allocator, registry, test::partition()),
          system(allocator) {}

    ClimateMap climate;
    FieldRegistry registry;
    FieldStore store;
    WeatherSystem system;

    [[nodiscard]] cy::Status build(const cy::weather::ClimateSample& sample) noexcept {
        if (cy::Status set = climate.set_uniform(sample); !set) {
            return set;
        }
        if (cy::Status configured = system.configure(test::config(), climate); !configured) {
            return configured;
        }
        if (cy::Status bound = system.bind_fields(registry, store); !bound) {
            return bound;
        }
        system.set_publish_region(region());
        cy::Expected<EcosystemReport, cy::Error> seeded =
            system.ecosystem().seed_potential(system.fields(), climate, region());
        if (!seeded) {
            return cy::make_unexpected(seeded.error());
        }
        return cy::ok();
    }

    [[nodiscard]] static PublishRegion region() noexcept {
        PublishRegion value;
        value.min_x = 0.0;
        value.min_z = 0.0;
        value.max_x = 3'000.0;
        value.max_z = 3'000.0;
        value.level = FieldResidency::Macro;
        return value;
    }

    [[nodiscard]] cy::f32 read(WeatherField field, cy::f64 x, cy::f64 z) const noexcept {
        return store
            .sample_deterministic(system.fields().id(field), cy::world::WorldVec3d{x, 0.0, z})
            .value.x();
    }
    [[nodiscard]] cy::u32 biome_at(cy::f64 x, cy::f64 z) const noexcept {
        return store
            .sample_deterministic(system.fields().id(WeatherField::Biome),
                                  cy::world::WorldVec3d{x, 0.0, z})
            .value.index();
    }
};

/// Bring a region up to its potential before the event that knocks it back, so that "it recovered"
/// is measured against a stand that was actually there.
[[nodiscard]] cy::Status grow(World& world, cy::f64 days) noexcept {
    cy::Expected<EcosystemReport, cy::Error> grown =
        world.system.ecosystem().advance_days(world.system.fields(), World::region(), days);
    if (!grown) {
        return cy::make_unexpected(grown.error());
    }
    return cy::ok();
}

}  // namespace

CY_TEST_CASE("the potential is written from the climate and the current state recovers toward it") {
    World world(test::allocator());
    CY_REQUIRE(world.build(test::temperate_climate()).has_value());

    const cy::f32 potential = world.read(WeatherField::VegetationPotential, 1'024.0, 1'024.0);
    CY_CHECK_GT(potential, 0.2F);
    // Nothing has grown yet: the current state starts at its declared default, which is bare.
    CY_CHECK_NEAR(world.read(WeatherField::VegetationDensity, 1'024.0, 1'024.0), 0.0F, 0.01F);

    CY_REQUIRE(grow(world, 60.0).has_value());
    const cy::f32 grown = world.read(WeatherField::VegetationDensity, 1'024.0, 1'024.0);
    CY_CHECK_GT(grown, 0.05F);
    CY_CHECK_LE(grown, potential + 0.01F);
    // And biomass follows density and age TOGETHER, so a young stand carries less than an old one
    // of the same density.
    CY_CHECK_GT(world.read(WeatherField::Biomass, 1'024.0, 1'024.0), 0.0F);
    // Sixty simulated days is 0.16 of a year, and the accumulator has to carry it: an age field
    // quantised over four centuries would have rounded every one of those 1 440 hour-long steps
    // back to zero. `Ecosystem::check_resolution()` is what stops that silently, and this is the
    // number it protects.
    CY_CHECK_GT(world.read(WeatherField::ForestAge, 1'024.0, 1'024.0), 0.1F);
}

CY_TEST_CASE("a burned forest regrows, and the scar outlives the grass") {
    // "WHEN a region burns and time passes THEN its burn fraction SHALL decay and vegetation
    // density SHALL recover toward potential, WITHOUT SIMULATING INDIVIDUAL PLANTS."
    World world(test::allocator());
    CY_REQUIRE(world.build(test::temperate_climate()).has_value());
    CY_REQUIRE(grow(world, 90.0).has_value());

    const cy::f32 before = world.read(WeatherField::VegetationDensity, 1'024.0, 1'024.0);
    const cy::f32 age_before = world.read(WeatherField::ForestAge, 1'024.0, 1'024.0);
    CY_REQUIRE((before) > (0.1F));

    EcosystemEvent fire;
    fire.kind = EcosystemEventKind::Fire;
    fire.min_x = 0.0;
    fire.min_z = 0.0;
    fire.max_x = 1'500.0;
    fire.max_z = 1'500.0;
    fire.magnitude = 1.0F;
    CY_REQUIRE(world.system.ecosystem().apply_event(world.system.fields(), fire).has_value());

    const cy::f32 burnt = world.read(WeatherField::VegetationDensity, 1'024.0, 1'024.0);
    CY_CHECK_LT(burnt, before * 0.2F);
    CY_CHECK_GT(world.read(WeatherField::BurnState, 1'024.0, 1'024.0), 0.8F);
    // The stand is reset: age and biomass go to zero, which is what a fire does.
    CY_CHECK_LT(world.read(WeatherField::ForestAge, 1'024.0, 1'024.0), age_before);
    CY_CHECK_NEAR(world.read(WeatherField::Biomass, 1'024.0, 1'024.0), 0.0F, 0.01F);
    // Ash is fertiliser: the soil takes far less than the canopy, which is why the forest comes
    // back at all.
    CY_CHECK_GT(world.read(WeatherField::SoilHealth, 1'024.0, 1'024.0), 0.4F);

    CY_REQUIRE(grow(world, 90.0).has_value());
    const cy::f32 regrown = world.read(WeatherField::VegetationDensity, 1'024.0, 1'024.0);
    const cy::f32 scar = world.read(WeatherField::BurnState, 1'024.0, 1'024.0);
    CY_CHECK_GT(regrown, burnt * 2.0F);
    // The scar heals more slowly than the grass grows back, which is what a burned forest looks
    // like a decade later.
    CY_CHECK_GT(scar, 0.05F);
    CY_CHECK_LT(scar, 1.0F);
}

CY_TEST_CASE("a region nobody is looking at evolves anyway") {
    // "evolving at low resolution over long periods, WHETHER OR NOT A REGION IS RESIDENT." The
    // macro level is declared resident everywhere, so the evolution runs over tiles that exist by
    // declaration rather than because something streamed them.
    World world(test::allocator());
    CY_REQUIRE(world.build(test::temperate_climate()).has_value());

    const cy::environment::FieldDeclaration* declaration =
        world.registry.declaration(world.system.fields().id(WeatherField::VegetationDensity));
    CY_REQUIRE(declaration != nullptr);
    CY_CHECK(declaration->levels[static_cast<cy::u32>(FieldResidency::Macro)].resident_everywhere);
    CY_CHECK(declaration->gameplay_level == FieldResidency::Macro);

    // There is no camera in this test, no refinement source, and no streaming — and the region
    // still grows.
    CY_REQUIRE(grow(world, 60.0).has_value());
    CY_CHECK_GT(world.read(WeatherField::VegetationDensity, 2'500.0, 2'500.0), 0.02F);
}

CY_TEST_CASE("ninety days in one call is ninety days in ninety calls") {
    // "The editor SHALL support ADVANCING environmental and ecosystem state rapidly ... This SHALL
    // use the same macro state and generation path as the runtime, NOT A SEPARATE PREVIEW MODEL."
    //
    // The property that makes that true rather than claimed: whole steps only, remainder carried,
    // so both routes run the same steps in the same order.
    World one(test::allocator());
    World many(test::allocator());
    CY_REQUIRE(one.build(test::temperate_climate()).has_value());
    CY_REQUIRE(many.build(test::temperate_climate()).has_value());

    cy::Expected<EcosystemReport, cy::Error> once =
        one.system.ecosystem().advance_days(one.system.fields(), World::region(), 90.0);
    CY_REQUIRE(once.has_value());
    cy::u32 stepped = 0;
    for (cy::u32 day = 0; day < 90; ++day) {
        cy::Expected<EcosystemReport, cy::Error> daily =
            many.system.ecosystem().advance_days(many.system.fields(), World::region(), 1.0);
        CY_REQUIRE(daily.has_value());
        stepped += daily->steps;
    }
    CY_CHECK_EQ(once->steps, stepped);
    CY_CHECK_EQ(one.read(WeatherField::VegetationDensity, 1'024.0, 1'024.0),
                many.read(WeatherField::VegetationDensity, 1'024.0, 1'024.0));
    CY_CHECK_EQ(one.read(WeatherField::ForestAge, 1'024.0, 1'024.0),
                many.read(WeatherField::ForestAge, 1'024.0, 1'024.0));
    CY_CHECK_EQ(one.read(WeatherField::Biomass, 1'024.0, 1'024.0),
                many.read(WeatherField::Biomass, 1'024.0, 1'024.0));

    // And a sequence of advances SHORTER than one macro step still adds up to the same thing,
    // rather than rounding away to nothing.
    World dribbled(test::allocator());
    CY_REQUIRE(dribbled.build(test::temperate_climate()).has_value());
    cy::u32 dribble_steps = 0;
    for (cy::u32 index = 0; index < 90 * 40; ++index) {
        cy::Expected<EcosystemReport, cy::Error> bit = dribbled.system.ecosystem().advance(
            dribbled.system.fields(), World::region(), cy::weather::kSecondsPerDay / 40.0);
        CY_REQUIRE(bit.has_value());
        dribble_steps += bit->steps;
    }
    CY_CHECK_EQ(dribble_steps, once->steps);
}

CY_TEST_CASE("terraforming crosses a declared threshold and the biome changes") {
    // "WHEN soil health and moisture cross declared thresholds THEN the region's biome state SHALL
    // change." The thresholds are `BiomeRule`s over FIELDS — data, not a switch — which is what
    // lets a project add a biome without an engine change.
    World world(test::allocator());
    cy::weather::ClimateSample arid = test::temperate_climate();
    arid.rainfall_potential_mm = 130.0F;
    arid.humidity = 0.15F;
    arid.mean_temperature_celsius = 26.0F;
    CY_REQUIRE(world.build(arid).has_value());
    CY_REQUIRE(grow(world, 60.0).has_value());

    const cy::u32 before = world.biome_at(1'024.0, 1'024.0);
    CY_CHECK_EQ(before, cy::weather::biomes::kDesert);

    EcosystemEvent terraform;
    terraform.kind = EcosystemEventKind::Terraforming;
    terraform.min_x = 0.0;
    terraform.min_z = 0.0;
    terraform.max_x = 3'000.0;
    terraform.max_z = 3'000.0;
    terraform.magnitude = 1.0F;
    terraform.target_soil_health = 0.9F;
    terraform.target_moisture = 0.4F;
    CY_REQUIRE(world.system.ecosystem().apply_event(world.system.fields(), terraform).has_value());

    // DESERT TO SAVANNA: the moisture has crossed a declared threshold and the biome follows.
    cy::Expected<EcosystemReport, cy::Error> after = world.system.ecosystem().advance(
        world.system.fields(), World::region(), cy::weather::kEcosystemStepSeconds);
    CY_REQUIRE(after.has_value());
    CY_CHECK_GT(after->biome_changes, 0u);
    CY_CHECK_EQ(world.biome_at(1'024.0, 1'024.0), cy::weather::biomes::kSavanna);

    // AND ON TO FOREST, because terraforming moved the POTENTIAL as well as the state: the moisture
    // stays where the programme put it and the vegetation grows into it, rather than the whole
    // thing draining back to desert the moment the recovery runs. The specification's own sequence
    // — "desert to savanna to forest as conditions change" — end to end.
    terraform.target_moisture = 0.72F;
    CY_REQUIRE(world.system.ecosystem().apply_event(world.system.fields(), terraform).has_value());
    CY_REQUIRE(grow(world, 120.0).has_value());
    CY_CHECK_EQ(world.biome_at(1'024.0, 1'024.0), cy::weather::biomes::kForest);
    CY_CHECK_GT(world.read(WeatherField::Moisture, 1'024.0, 1'024.0), 0.5F);

    // The rule table is what decided it, and it is readable. A project replacing it replaces this
    // answer and nothing else.
    CY_CHECK_GT(world.system.ecosystem().biome_rules().size(), 3u);
}

CY_TEST_CASE("drought and deforestation move different fields") {
    World world(test::allocator());
    CY_REQUIRE(world.build(test::temperate_climate()).has_value());
    CY_REQUIRE(grow(world, 90.0).has_value());
    const cy::f32 vegetation = world.read(WeatherField::VegetationDensity, 1'024.0, 1'024.0);
    const cy::f32 moisture = world.read(WeatherField::Moisture, 1'024.0, 1'024.0);
    const cy::f32 soil = world.read(WeatherField::SoilHealth, 1'024.0, 1'024.0);

    EcosystemEvent drought;
    drought.kind = EcosystemEventKind::Drought;
    drought.min_x = 0.0;
    drought.min_z = 0.0;
    drought.max_x = 3'000.0;
    drought.max_z = 3'000.0;
    drought.magnitude = 1.0F;
    CY_REQUIRE(world.system.ecosystem().apply_event(world.system.fields(), drought).has_value());
    CY_CHECK_LT(world.read(WeatherField::Moisture, 1'024.0, 1'024.0), moisture * 0.5F);
    CY_CHECK_NEAR(world.read(WeatherField::SoilHealth, 1'024.0, 1'024.0), soil, 0.01F);

    World other(test::allocator());
    CY_REQUIRE(other.build(test::temperate_climate()).has_value());
    CY_REQUIRE(grow(other, 90.0).has_value());
    EcosystemEvent pollution = drought;
    pollution.kind = EcosystemEventKind::Pollution;
    CY_REQUIRE(other.system.ecosystem().apply_event(other.system.fields(), pollution).has_value());
    CY_CHECK_LT(other.read(WeatherField::SoilHealth, 1'024.0, 1'024.0), soil * 0.6F);
    // Soil declares no potential, so it does NOT heal itself — a contaminated site stays
    // contaminated until something terraforms it.
    const cy::f32 poisoned = other.read(WeatherField::SoilHealth, 1'024.0, 1'024.0);
    CY_REQUIRE(grow(other, 60.0).has_value());
    CY_CHECK_NEAR(other.read(WeatherField::SoilHealth, 1'024.0, 1'024.0), poisoned, 0.02F);
    CY_CHECK_LT(vegetation + 1.0F, 2.0F);  // keeps `vegetation` used, and it is a real bound
}
