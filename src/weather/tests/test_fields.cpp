// Weather as a producer into CyberField: the declarations, the SECOND PRODUCER REFUSAL, the
// firewall, and the publication that is the whole of how weather reaches the world. M10 tasks 3.1
// and 3.2.
//
// An integration suite: a real `environment::FieldRegistry`, a real `FieldStore`, real producer
// tokens and real tiles. What it measures is that a consumer learns everything it needs about the
// weather by sampling fields — and that a second producer for one of them fails, naming both.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`: the all-or-nothing
// pre-check in `WeatherFields::claim()` was deleted, leaving the second loop to claim fields until
// it hit the contested one. "a second producer for wetness is refused, naming both" still failed
// correctly — but "a refused claim leaves the registry as it was" went red, reporting `temperature`
// produced by weather in a configuration that had been refused. The pre-check was restored.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/weather/fields.h>
#include <cy/weather/system.h>

#include <cmath>

#include "fixtures.h"

namespace test = cy::weather::test;
using cy::environment::FieldDeclaration;
using cy::environment::FieldReader;
using cy::environment::FieldRegistry;
using cy::environment::FieldResidency;
using cy::environment::FieldSample;
using cy::environment::FieldStore;
using cy::environment::FirewallViolation;
using cy::weather::ClimateMap;
using cy::weather::WeatherField;
using cy::weather::WeatherFieldOptions;
using cy::weather::WeatherFields;
using cy::weather::WeatherSystem;
using cy::weather::WetnessSource;

namespace {

[[nodiscard]] ClimateMap& cold_climate() {
    static ClimateMap map(test::allocator());
    static const bool ready = [] { return map.set_uniform(test::cold_climate()).has_value(); }();
    (void)ready;
    return map;
}

[[nodiscard]] ClimateMap& uniform_climate() {
    static ClimateMap map(test::allocator());
    static const bool ready = [] {
        return map.set_uniform(test::temperate_climate()).has_value();
    }();
    (void)ready;
    return map;
}

[[nodiscard]] bool contains(const char* text, const char* needle) noexcept {
    if (text == nullptr || needle == nullptr) {
        return false;
    }
    for (const char* start = text; *start != '\0'; ++start) {
        const char* a = start;
        const char* b = needle;
        while (*b != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

}  // namespace

CY_TEST_CASE(
    "every declared field is legal, and the two wind fields are on opposite sides of the "
    "firewall") {
    const WeatherFieldOptions options;
    for (cy::u32 index = 0; index < cy::weather::kWeatherFieldCount; ++index) {
        const auto field = static_cast<WeatherField>(index);
        const FieldDeclaration declaration = cy::weather::weather_field_declaration(field, options);
        // The substrate's own validator, on every row of the table. A declaration this refuses
        // would fail at `declare()` in a project and not in a suite.
        CY_CHECK(cy::environment::validate_declaration(declaration) ==
                 cy::environment::DeclarationProblem::None);
    }

    const FieldDeclaration wind =
        cy::weather::weather_field_declaration(WeatherField::Wind, options);
    const FieldDeclaration turbulence =
        cy::weather::weather_field_declaration(WeatherField::WindTurbulence, options);
    CY_CHECK(wind.classification == cy::determinism::SimulationClass::Authoritative);
    CY_CHECK(turbulence.classification == cy::determinism::SimulationClass::Presentation);
    CY_CHECK(wind.gameplay_visible());
    CY_CHECK_FALSE(turbulence.gameplay_visible());
    // Both are volumetric, because a canyon's wind at ground level and fifty metres up are
    // different winds.
    CY_CHECK_GT(wind.vertical_cells, 1u);
    CY_CHECK_EQ(wind.vertical_cells, turbulence.vertical_cells);

    // The precipitation TYPE is a category sampled nearest — an interpolated one would name nothing
    // — and the substrate refuses the other pairing, which is why this is checked and not
    // remembered.
    const FieldDeclaration type =
        cy::weather::weather_field_declaration(WeatherField::PrecipitationType, options);
    CY_CHECK(type.type == cy::environment::FieldType::Category);
    CY_CHECK(type.interpolation == cy::environment::FieldInterpolation::Nearest);

    // The accumulation and ecosystem halves are persistent, so a region that was snowed on and
    // unloaded keeps its snow.
    CY_CHECK(cy::weather::weather_field_declaration(WeatherField::SnowDepth, options).persistent);
    CY_CHECK(cy::weather::weather_field_declaration(WeatherField::BurnState, options).persistent);
    // And vegetation declares a potential, which is what makes the regrowth the substrate's.
    const FieldDeclaration vegetation =
        cy::weather::weather_field_declaration(WeatherField::VegetationDensity, options);
    CY_CHECK(vegetation.potential.is_valid());
    CY_CHECK_GT(vegetation.recovery_per_second, 0.0F);
}

CY_TEST_CASE(
    "weather claims its fields and a second producer for one of them is refused, naming "
    "both") {
    // THE EXIT CRITERION `environment-fields` sets and this row has to meet from the other side:
    // "a second producer for the same field fails, naming both producers."
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());

    WeatherFields fields(test::allocator());
    WeatherFieldOptions options;
    CY_REQUIRE(fields.declare(registry, options).has_value());
    CY_REQUIRE(fields.claim(registry, store).has_value());
    CY_CHECK(fields.claimed());

    // Somebody else — the water row, in the configuration where it owns the shore's wetness —
    // tries to produce `wetness` too.
    cy::Expected<cy::environment::ProducerToken, cy::Error> refused = registry.claim(
        fields.id(WeatherField::Wetness), "water.shoreline", cy::environment::ProducerKind::System);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(contains(refused.error().message, "weather.system"));
    CY_CHECK(contains(refused.error().message, "water.shoreline"));
    const cy::environment::ProducerConflict& conflict = registry.last_conflict();
    CY_CHECK(conflict.occurred());
    CY_CHECK(contains(conflict.incumbent, "weather"));
    CY_CHECK(contains(conflict.challenger, "water"));
}

CY_TEST_CASE("a refused claim leaves the registry as it was") {
    // ALL OR NOTHING. A claim that failed halfway would leave some of weather's fields produced by
    // weather in a configuration that was refused — a state in which the refusal is correct and the
    // registry describes something nobody chose.
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());

    // Another row gets `wetness` first.
    WeatherFields fields(test::allocator());
    CY_REQUIRE(fields.declare(registry, WeatherFieldOptions{}).has_value());
    cy::Expected<cy::environment::ProducerToken, cy::Error> shore = registry.claim(
        fields.id(WeatherField::Wetness), "water.shoreline", cy::environment::ProducerKind::System);
    CY_REQUIRE(shore.has_value());

    CY_CHECK_FALSE(fields.claim(registry, store).has_value());
    CY_CHECK_FALSE(fields.claimed());

    // Nothing else was taken on the way to the refusal.
    for (cy::u32 index = 0; index < cy::weather::kWeatherFieldCount; ++index) {
        const auto field = static_cast<WeatherField>(index);
        if (field == WeatherField::Wetness) {
            continue;
        }
        const cy::environment::FieldRecord* record = registry.find(fields.id(field));
        CY_REQUIRE(record != nullptr);
        CY_CHECK_FALSE(record->claimed);
    }
}

CY_TEST_CASE("a project whose water row owns wetness configures weather to compose the shore") {
    // fields.h's header note, as a running configuration: `ComposeShore` means weather still claims
    // `wetness` and READS `water-shore-wetness`, which is exactly what src/water/'s
    // `WetnessOwner::External` publishes for it.
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());

    // The water row's side: it declares and claims the shore field.
    FieldDeclaration shore;
    shore.name = cy::weather::fields::kShoreWetness;
    shore.unit = "fraction";
    shore.semantics = "the shore's contribution to wetness";
    shore.encoding = cy::environment::FieldEncoding::UNorm8;
    shore.classification = cy::determinism::SimulationClass::Persistent;
    shore.gameplay_level = FieldResidency::Macro;
    shore.levels[static_cast<cy::u32>(FieldResidency::Macro)] =
        cy::environment::FieldLevel{1'024.0F, true};
    CY_REQUIRE(registry.declare(shore).has_value());
    cy::Expected<cy::environment::ProducerToken, cy::Error> shore_token =
        registry.claim(shore.id(), "water.shoreline", cy::environment::ProducerKind::System);
    CY_REQUIRE(shore_token.has_value());

    WeatherFieldOptions options;
    options.wetness = WetnessSource::ComposeShore;
    WeatherFields fields(test::allocator());
    CY_REQUIRE(fields.declare(registry, options).has_value());
    CY_REQUIRE(fields.claim(registry, store).has_value());
    CY_REQUIRE(fields.declare_consumers(registry).has_value());

    // Both rows coexist, each producing its own field, and the firewall is clean over the whole
    // configuration.
    cy::Array<FirewallViolation> violations(test::allocator());
    CY_REQUIRE(registry.validate(violations).has_value());
    CY_CHECK_EQ(violations.size(), 0u);
}

CY_TEST_CASE("gameplay may read the wind and may not read the turbulence") {
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());
    WeatherFields fields(test::allocator());
    CY_REQUIRE(fields.declare(registry, WeatherFieldOptions{}).has_value());
    CY_REQUIRE(fields.claim(registry, store).has_value());

    // At the point of use: `FieldReader::open()` calls `determinism::may_read()`, which is the
    // engine's own firewall predicate and not a second one.
    CY_CHECK(FieldReader::open(store, fields.id(WeatherField::Wind),
                               cy::determinism::SimulationClass::Authoritative)
                 .has_value());
    CY_CHECK_FALSE(FieldReader::open(store, fields.id(WeatherField::WindTurbulence),
                                     cy::determinism::SimulationClass::Authoritative)
                       .has_value());
    CY_CHECK(FieldReader::open(store, fields.id(WeatherField::WindTurbulence),
                               cy::determinism::SimulationClass::Presentation)
                 .has_value());

    // And over the whole configuration, before a frame runs: a gameplay system that DECLARED it
    // reads the turbulence is reported by `validate()`.
    CY_REQUIRE(registry
                   .declare_consumer("gameplay.projectile",
                                     cy::determinism::SimulationClass::Authoritative,
                                     fields.id(WeatherField::WindTurbulence))
                   .has_value());
    cy::Array<FirewallViolation> violations(test::allocator());
    CY_REQUIRE(registry.validate(violations).has_value());
    CY_REQUIRE_EQ(violations.size(), 1u);
    CY_CHECK(contains(violations[0].consumer, "projectile"));
}

CY_TEST_CASE("weather publishes state and a consumer reads it without calling weather") {
    // "Consumers SHALL sample the fields they need at the fidelity they need: materials sample
    // wetness, foliage samples wind, water samples precipitation, audio samples wind and rain,
    // artificial intelligence samples visibility. There SHALL NOT be a central weather component
    // that pushes state to subsystems."
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());

    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(system.bind_fields(registry, store).has_value());
    system.set_publish_region(test::small_region());

    // Drive it toward a storm so there is something to publish.
    CY_REQUIRE(
        system.director()
            .apply(cy::weather::preset_storm(), cy::weather::TransitionProfile::uniform(60.0F), 0)
            .has_value());
    for (cy::u32 tick = 1; tick <= 60; ++tick) {
        CY_REQUIRE(system.advance(test::at_tick(tick), 60.0).has_value());
    }

    // A FOLIAGE-shaped consumer, reading wind and nothing else.
    cy::Expected<FieldReader, cy::Error> foliage =
        FieldReader::open(store, system.fields().id(WeatherField::Wind),
                          cy::determinism::SimulationClass::Presentation);
    CY_REQUIRE(foliage.has_value());
    const FieldSample wind = foliage->sample(cy::world::WorldVec3d{512.0, 16.0, 512.0});
    CY_CHECK(wind.resolved);
    CY_CHECK_GT(std::sqrt((wind.value.x() * wind.value.x()) + (wind.value.z() * wind.value.z())),
                0.1F);

    // AN AI-shaped consumer, reading visibility.
    cy::Expected<FieldReader, cy::Error> ai =
        FieldReader::open(store, system.fields().id(WeatherField::Visibility),
                          cy::determinism::SimulationClass::Authoritative);
    CY_REQUIRE(ai.has_value());
    const FieldSample visibility = ai->sample(cy::world::WorldVec3d{512.0, 0.0, 512.0});
    CY_CHECK(visibility.resolved);
    CY_CHECK_GT(visibility.value.x(), 0.0F);
    // The storm has cut it well below the clear-weather value the climate implies: a bound, not a
    // magic number, because the exact figure is the hierarchy's weighting and that is not what this
    // case is about.
    CY_CHECK_LT(visibility.value.x(), 20'000.0F);

    // WATER-shaped: the precipitation rate and its type.
    const FieldSample rate =
        store.sample_deterministic(system.fields().id(WeatherField::PrecipitationRate),
                                   cy::world::WorldVec3d{512.0, 0.0, 512.0});
    CY_CHECK_GT(rate.value.x(), 0.5F);
    const FieldSample kind =
        store.sample_deterministic(system.fields().id(WeatherField::PrecipitationType),
                                   cy::world::WorldVec3d{512.0, 0.0, 512.0});
    CY_CHECK_EQ(kind.value.index(), static_cast<cy::u32>(cy::weather::PrecipitationType::Rain));
}

CY_TEST_CASE("the published wind is the authoritative half and the turbulence is the rest") {
    // The publication's own claim: `wind` carries `base + gust` and `wind-turbulence` carries the
    // residual, so a gameplay consumer of `wind` cannot see a value that depended on presentation.
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());
    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(system.bind_fields(registry, store).has_value());
    system.set_publish_region(test::small_region());
    CY_REQUIRE(system.advance(test::at_tick(1), 60.0).has_value());

    const cy::world::WorldVec3d at{512.0, 16.0, 512.0};
    const FieldSample published =
        store.sample_deterministic(system.fields().id(WeatherField::Wind), at);
    const cy::weather::WindSample composed =
        system
            .sample(at, cy::weather::SampleQuality::Gameplay,
                    cy::determinism::SimulationClass::Authoritative)
            .wind;
    // The field is quantised by nothing — the wind field is f32 — so the agreement is to the
    // lattice's own interpolation, not to a storage step.
    CY_CHECK_NEAR(published.value.x(), composed.authoritative().x, 0.15F);
    CY_CHECK_NEAR(published.value.z(), composed.authoritative().z, 0.15F);

    const FieldSample residual = store.sample(system.fields().id(WeatherField::WindTurbulence), at);
    CY_CHECK(residual.resolved);
}

CY_TEST_CASE("a publication that would exceed the field-update budget is deferred, not trimmed") {
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());
    cy::weather::WeatherConfig config = test::config();
    config.budget.field_points_per_tick = 4;
    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(config, uniform_climate()).has_value());
    CY_REQUIRE(system.bind_fields(registry, store).has_value());
    system.set_publish_region(test::small_region());

    cy::Expected<cy::weather::WeatherTickReport, cy::Error> report =
        system.advance(test::at_tick(1), 60.0);
    CY_REQUIRE(report.has_value());
    CY_CHECK(report->publication_throttled);
    CY_CHECK_EQ(report->atmosphere.lattice_points, 0u);

    // Raise the allowance and the same region goes out whole.
    config.budget.field_points_per_tick = 1'000'000;
    WeatherSystem generous(test::allocator());
    FieldRegistry other_registry(test::allocator());
    FieldStore other_store(test::allocator(), other_registry, test::partition());
    CY_REQUIRE(generous.configure(config, uniform_climate()).has_value());
    CY_REQUIRE(generous.bind_fields(other_registry, other_store).has_value());
    generous.set_publish_region(test::small_region());
    cy::Expected<cy::weather::WeatherTickReport, cy::Error> full =
        generous.advance(test::at_tick(1), 60.0);
    CY_REQUIRE(full.has_value());
    CY_CHECK_FALSE(full->publication_throttled);
    CY_CHECK_GT(full->atmosphere.lattice_points, 0u);
    CY_CHECK_EQ(full->atmosphere.fields_written, 6u);
}

CY_TEST_CASE("rain accumulates into wetness and the sun takes it away again") {
    // "WHEN rain stops and the sun comes out THEN wetness SHALL decay at a rate driven by
    // temperature, sun, and wind." And "WHEN it rains THEN the wetness field SHALL change and
    // materials SHALL sample it, WITH NO PER-MATERIAL UPDATE" — there is no material in this test
    // because there is nowhere in the module for one to be.
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());
    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(test::config(), uniform_climate()).has_value());
    CY_REQUIRE(system.bind_fields(registry, store).has_value());
    system.set_publish_region(test::small_region());

    CY_REQUIRE(
        system.director()
            .apply(cy::weather::preset_storm(), cy::weather::TransitionProfile::uniform(30.0F), 0)
            .has_value());
    for (cy::u32 tick = 1; tick <= 60; ++tick) {
        CY_REQUIRE(system.advance(test::at_tick(tick), 60.0).has_value());
    }
    const cy::world::WorldVec3d at{512.0, 0.0, 512.0};
    const cy::f32 wet =
        store.sample_deterministic(system.fields().id(WeatherField::Wetness), at).value.x();
    CY_CHECK_GT(wet, 0.2F);

    // Now the sun comes out. Clear is warm, dry and lightly windy — all three of the drivers the
    // requirement names.
    CY_REQUIRE(
        system.director()
            .apply(cy::weather::preset_clear(), cy::weather::TransitionProfile::uniform(60.0F), 60)
            .has_value());
    for (cy::u32 tick = 61; tick <= 400; ++tick) {
        CY_REQUIRE(system.advance(test::at_tick(tick), 60.0).has_value());
    }
    const cy::f32 dried =
        store.sample_deterministic(system.fields().id(WeatherField::Wetness), at).value.x();
    CY_CHECK_LT(dried, wet);
}

CY_TEST_CASE("snow accumulates as a field and melts, and it is never geometry") {
    // "WHEN snow accumulates THEN it SHALL be a field composited into surfaces, NOT A MODIFICATION
    // OF TERRAIN GEOMETRY." The enforcement is that the only thing this module writes is a
    // `FieldValue` — there is no mesh, no height and no terrain handle anywhere in it.
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());
    WeatherSystem system(test::allocator());
    // A COLD PLACE, not merely a cold preset: a regional target is half the place's own climate, so
    // a snowstorm over a temperate world makes cold rain. See fixtures.h.
    CY_REQUIRE(system.configure(test::config(), cold_climate()).has_value());
    CY_REQUIRE(system.bind_fields(registry, store).has_value());
    system.set_publish_region(test::small_region());

    CY_REQUIRE(system.director()
                   .apply(cy::weather::preset_snowstorm(),
                          cy::weather::TransitionProfile::uniform(60.0F), 0)
                   .has_value());
    for (cy::u32 tick = 1; tick <= 200; ++tick) {
        CY_REQUIRE(system.advance(test::at_tick(tick), 120.0).has_value());
    }
    const cy::world::WorldVec3d at{512.0, 0.0, 512.0};
    const cy::f32 depth =
        store.sample_deterministic(system.fields().id(WeatherField::SnowDepth), at).value.x();
    CY_CHECK_GT(depth, 0.001F);

    // A thaw: warm and sunny.
    CY_REQUIRE(
        system.director()
            .apply(cy::weather::preset_clear(), cy::weather::TransitionProfile::uniform(60.0F), 200)
            .has_value());
    for (cy::u32 tick = 201; tick <= 900; ++tick) {
        CY_REQUIRE(system.advance(test::at_tick(tick), 120.0).has_value());
    }
    const cy::f32 melted =
        store.sample_deterministic(system.fields().id(WeatherField::SnowDepth), at).value.x();
    CY_CHECK_LT(melted, depth);
}
