// The shoreline water OWNS, and the four environment fields it produces. M10 task 2.3, and
// `water`'s "Shoreline" requirement — the one whose whole point is the DIRECTION of the dependency.
//
// This is an integration suite: a real `environment::FieldRegistry`, a real `FieldStore`, real
// producer tokens and real tiles. What it measures is that terrain can learn everything it needs
// about water by sampling fields, and that water learns everything it needs about terrain through
// one callback — so neither module names the other.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`: the chamfer sweep's
// backward pass was deleted, which leaves a distance field that is correct only for water lying up
// and to the left of a point. "water distance is a distance, measured from both sides" went red on
// the seaward-facing assertion, reporting the saturated default where the shore was two cells away.
// The pass was restored.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/water/shoreline.h>
#include <cy/water/system.h>

#include <cmath>

#include "fixtures.h"

namespace test = cy::water::test;
using cy::environment::FieldRegistry;
using cy::environment::FieldResidency;
using cy::environment::FieldStore;
using cy::water::WaterFieldOptions;
using cy::water::WaterFields;
using cy::water::WaterSystem;
using cy::water::WetnessOwner;

namespace {

/// A beach: the ground rises one metre in ten, crossing sea level at x = 0. Everything to the west
/// is under water and everything to the east is dry land.
cy::f64 beach_bed(void*, cy::f64 x, cy::f64) noexcept {
    return x * 0.1;
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

/// A SPIT: land between two bodies of water. The ground is below sea level west of x = 0 and east
/// of x = 40, and above it in between — so a point on the spit is nearer the water on ONE of the
/// two sides, and which one depends on where it is.
///
/// This shape exists because a one-sided sweep is correct for a beach: with all the water to the
/// west, propagating distances only west-to-east gives the right answer everywhere, and a suite
/// built on a beach alone cannot tell a distance transform from half of one.
cy::f64 spit_bed(void*, cy::f64 x, cy::f64) noexcept {
    if (x < 0.0) {
        return x * 0.2;
    }
    if (x > 40.0) {
        return (40.0 - x) * 0.2;
    }
    return 2.0;
}

/// The `wetness` declaration a weather row would make. Water does not declare it when the owner is
/// external — the whole point of that setting is that another row owns the field — so a suite
/// standing in for weather has to declare it the way weather would.
[[nodiscard]] cy::environment::FieldDeclaration weather_wetness() noexcept {
    cy::environment::FieldDeclaration declaration;
    declaration.name = cy::environment::fields::kWetness;
    declaration.unit = "fraction";
    declaration.semantics = "surface wetness, 0 dry to 1 saturated";
    declaration.type = cy::environment::FieldType::Scalar;
    declaration.encoding = cy::environment::FieldEncoding::UNorm8;
    declaration.range_min = 0.0F;
    declaration.range_max = 1.0F;
    declaration.default_value = cy::environment::FieldValue::scalar(0.0F);
    declaration.levels[static_cast<cy::u32>(FieldResidency::Local)] =
        cy::environment::FieldLevel{2.0F, false};
    declaration.levels[static_cast<cy::u32>(FieldResidency::Macro)] =
        cy::environment::FieldLevel{64.0F, true};
    declaration.classification = cy::determinism::SimulationClass::Persistent;
    declaration.gameplay_level = FieldResidency::Macro;
    return declaration;
}

/// A flat sea at level zero, so the depth a field carries is exactly the bed's own depth.
[[nodiscard]] cy::water::WaterBodyDesc flat_sea() noexcept {
    cy::water::WaterBodyDesc desc = test::ocean_desc();
    desc.name = "test.flat-sea";
    desc.backend = cy::water::WaterBackend::Flat;
    return desc;
}

}  // namespace

CY_TEST_CASE("water writes depth, distance, flow and wetness, and terrain reads them") {
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());
    WaterSystem system(test::allocator(), test::partition());
    CY_REQUIRE(system.registry().add(flat_sea()).has_value());
    system.set_bed_source(&beach_bed, nullptr);

    WaterFieldOptions options;
    CY_REQUIRE(system.fields().declare(registry, options).has_value());
    CY_REQUIRE(system.fields().claim(registry, store).has_value());

    const cy::water::ShorelineInputs inputs = system.shoreline_inputs();
    CY_REQUIRE(
        system.fields().publish(inputs, FieldResidency::Local, -60.0, 0.0, 60.0, 16.0).has_value());
    CY_CHECK_GT(system.fields().last_published_points(), 0u);

    const cy::environment::FieldId depth = system.fields().depth_field();
    const cy::environment::FieldId distance = system.fields().distance_field();
    const cy::environment::FieldId wetness = system.fields().wetness_field();

    // TEN METRES OUT TO SEA: the bed is a metre down and the column says so.
    const cy::environment::FieldSample deep =
        store.sample(depth, cy::world::WorldVec3d{-30.0, 0.0, 7.0});
    CY_CHECK(deep.resolved);
    CY_CHECK_NEAR(deep.value.x(), 3.0F, 0.1F);

    // ON DRY LAND: no column at all, and the distance field measures how far the water is.
    const cy::environment::FieldSample dry =
        store.sample(depth, cy::world::WorldVec3d{30.0, 0.0, 7.0});
    CY_CHECK_NEAR(dry.value.x(), 0.0F, 0.05F);
    const cy::environment::FieldSample inland =
        store.sample(distance, cy::world::WorldVec3d{30.0, 0.0, 7.0});
    // Bounds rather than `CY_CHECK_NEAR`, whose third argument is a RELATIVE epsilon: thirty metres
    // from the waterline is thirty metres of water distance, to within a cell of the level.
    CY_CHECK_GT(inland.value.x(), 28.0F);
    CY_CHECK_LT(inland.value.x(), 32.0F);
    const cy::environment::FieldSample offshore =
        store.sample(distance, cy::world::WorldVec3d{-30.0, 0.0, 7.0});
    CY_CHECK_NEAR(offshore.value.x(), 0.0F, 0.05F);

    // A WET SHORE: saturated in the water, damp just above the waterline, dry inland. "the wetness
    // field SHALL be updated and the terrain material SHALL darken ... without terrain knowing
    // about waves."
    const cy::f32 in_water =
        store.sample(wetness, cy::world::WorldVec3d{-10.0, 0.0, 7.0}).value.x();
    const cy::f32 at_shore = store.sample(wetness, cy::world::WorldVec3d{2.0, 0.0, 7.0}).value.x();
    const cy::f32 far_inland =
        store.sample(wetness, cy::world::WorldVec3d{40.0, 0.0, 7.0}).value.x();
    CY_CHECK_NEAR(in_water, 1.0F, 0.02F);
    CY_CHECK_GT(at_shore, 0.1F);
    CY_CHECK_LT(at_shore, 1.0F);
    CY_CHECK_NEAR(far_inland, 0.0F, 0.02F);

    // AND THE DIRECTION. A terrain material is a declared CONSUMER of these fields; the firewall
    // validates the whole configuration and finds nothing, because reading is all terrain does.
    CY_REQUIRE(registry
                   .declare_consumer("terrain.material",
                                     cy::determinism::SimulationClass::Presentation, distance)
                   .has_value());
    CY_REQUIRE(registry
                   .declare_consumer("foliage.placement",
                                     cy::determinism::SimulationClass::Persistent, depth)
                   .has_value());
    cy::Array<cy::environment::FirewallViolation> violations(test::allocator());
    CY_REQUIRE(registry.validate(violations).has_value());
    CY_CHECK_EQ(violations.size(), 0u);
}

CY_TEST_CASE("water distance is a distance, measured from both sides") {
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());
    WaterSystem system(test::allocator(), test::partition());
    CY_REQUIRE(system.registry().add(flat_sea()).has_value());
    // A spit: water to the west AND to the east, land in between. See `spit_bed`.
    system.set_bed_source(&spit_bed, nullptr);

    WaterFieldOptions options;
    CY_REQUIRE(system.fields().declare(registry, options).has_value());
    CY_REQUIRE(system.fields().claim(registry, store).has_value());
    CY_REQUIRE(
        system.fields()
            .publish(system.shoreline_inputs(), FieldResidency::Local, -60.0, 0.0, 100.0, 16.0)
            .has_value());

    const cy::environment::FieldId distance = system.fields().distance_field();
    const auto at = [&](cy::f64 x) noexcept {
        return store.sample(distance, cy::world::WorldVec3d{x, 0.0, 7.0}).value.x();
    };

    // In the water, on both sides: zero.
    CY_CHECK_LT(at(-20.0), 0.01F);
    CY_CHECK_LT(at(60.0), 0.01F);

    // ON THE SPIT, NEAR ITS EASTERN SHORE. The water that is closest is to the EAST, four metres
    // away. Bounds rather than a tolerance, because `CY_CHECK_NEAR`'s third argument is a RELATIVE
    // epsilon and a generous one would accept the distance back to the western sea.
    //
    // These are the assertions the backward sweep exists for. A forward-only chamfer still leaks
    // some eastern information through its up-and-right diagonal — one cell per row, so with this
    // rectangle's eight rows it reports 7.07 m here and 21.2 m at x = 34 instead of 5 and 7. The
    // bounds below are inside that gap on purpose: a one-directional sweep fails them.
    CY_CHECK_GT(at(36.0), 4.0F);
    CY_CHECK_LT(at(36.0), 6.0F);
    CY_CHECK_LT(at(34.0), 9.0F);
    CY_CHECK_LT(at(32.0), 11.0F);

    // Near the western shore the nearest water is west, and the two halves meet in the middle.
    CY_CHECK_GT(at(4.0), 4.0F);
    CY_CHECK_LT(at(4.0), 6.0F);
    CY_CHECK_GT(at(20.0), at(4.0));
    CY_CHECK_GT(at(20.0), at(36.0));

    // AND THE FIELD IS SYMMETRIC about the spit's centre, because the spit is. A sweep that
    // measured one side better than the other would break this without breaking a single monotone
    // assertion above.
    for (cy::u32 step = 0; step <= 8; ++step) {
        const cy::f64 offset = static_cast<cy::f64>(step) * 2.0;
        const cy::f32 west = at(2.0 + offset);
        const cy::f32 east = at(38.0 - offset);
        CY_CHECK_LT(std::fabs(west - east), 0.51F);
    }

    // And it rises away from the water rather than saturating: metres, not a flag.
    cy::f32 previous = -1.0F;
    for (cy::u32 step = 1; step <= 5; ++step) {
        const cy::f32 value = at(static_cast<cy::f64>(step) * 3.0);
        CY_CHECK_LT(value, options.max_water_distance_metres);
        CY_CHECK_GT(value, previous);
        previous = value;
    }
}

CY_TEST_CASE("a river writes its flow into the field, and debris downstream reads it") {
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());
    WaterSystem system(test::allocator(), test::partition());
    CY_REQUIRE(system.registry().add(test::river_desc()).has_value());
    CY_REQUIRE(test::author_straight_river(system.rivers(), 10.0F).has_value());
    cy::water::RiverBuildReport report;
    CY_REQUIRE(system.rivers().build(report).has_value());

    WaterFieldOptions options;
    CY_REQUIRE(system.fields().declare(registry, options).has_value());
    CY_REQUIRE(system.fields().claim(registry, store).has_value());
    CY_REQUIRE(
        system.fields()
            .publish(system.shoreline_inputs(), FieldResidency::Local, 40.0, 92.0, 60.0, 108.0)
            .has_value());

    const cy::environment::FieldSample flow =
        store.sample(system.fields().flow_field(), cy::world::WorldVec3d{50.0, 0.0, 100.0});
    CY_CHECK(flow.resolved);
    // "Rivers SHALL write flow and water distance into environment fields, so terrain materials,
    // foliage, audio, and gameplay read them WITHOUT QUERYING THE WATER SYSTEM."
    CY_CHECK_GT(flow.value.x(), 0.5F);
    CY_CHECK_NEAR(flow.value.y(), 0.0F, 0.2F);

    // On the bank there is no flow, so the field distinguishes the channel from its surroundings.
    const cy::environment::FieldSample bank =
        store.sample(system.fields().flow_field(), cy::world::WorldVec3d{50.0, 0.0, 107.0});
    CY_CHECK_NEAR(bank.value.x(), 0.0F, 0.2F);
}

CY_TEST_CASE("the gameplay-visible answer does not depend on what streamed") {
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());
    WaterSystem system(test::allocator(), test::partition());
    CY_REQUIRE(system.registry().add(flat_sea()).has_value());
    system.set_bed_source(&beach_bed, nullptr);

    WaterFieldOptions options;
    CY_REQUIRE(system.fields().declare(registry, options).has_value());
    CY_REQUIRE(system.fields().claim(registry, store).has_value());

    // Only the macro level is published: the level a gameplay-visible field declares, and the one
    // that has to exist everywhere.
    CY_REQUIRE(
        system.fields()
            .publish(system.shoreline_inputs(), FieldResidency::Macro, -256.0, 0.0, 256.0, 128.0)
            .has_value());

    const cy::environment::FieldId depth = system.fields().depth_field();
    const cy::world::WorldVec3d probe{-100.0, 0.0, 40.0};
    const cy::f32 before = store.sample_deterministic(depth, probe).value.x();
    CY_CHECK_GT(before, 0.0F);

    // Now the local level arrives for part of the world — which is exactly what streaming does.
    CY_REQUIRE(
        system.fields()
            .publish(system.shoreline_inputs(), FieldResidency::Local, -120.0, 0.0, -80.0, 64.0)
            .has_value());

    // The presentation path follows the finer data...
    const cy::environment::FieldSample presentation = store.sample(depth, probe);
    CY_CHECK_EQ(static_cast<int>(presentation.level), static_cast<int>(FieldResidency::Local));
    // ...and the gameplay path does not move at all. A boat's buoyancy and a swimmer's state are
    // therefore the same on a machine that streamed this cell and one that did not.
    CY_CHECK_EQ(store.sample_deterministic(depth, probe).value.x(), before);

    // And the reader's own class decides nothing: the FIELD decides, and this one is authoritative.
    const auto reader = cy::environment::FieldReader::open(
        store, depth, cy::determinism::SimulationClass::Presentation);
    CY_REQUIRE(reader.has_value());
    CY_CHECK(reader->deterministic());
    CY_CHECK_EQ(reader->sample(probe).value.x(), before);
}

CY_TEST_CASE("a second producer for wetness is refused, naming both") {
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());
    WaterSystem system(test::allocator(), test::partition());

    WaterFieldOptions options;
    options.wetness_owner = WetnessOwner::Water;
    CY_REQUIRE(system.fields().declare(registry, options).has_value());

    // Weather gets there first — which on a world with both rows live is the ordinary case.
    const auto weather =
        registry.claim(cy::environment::field_id(cy::environment::fields::kWetness),
                       "weather.precipitation", cy::environment::ProducerKind::System);
    CY_REQUIRE(weather.has_value());

    const cy::Status claimed = system.fields().claim(registry, store);
    CY_REQUIRE_FALSE(claimed.has_value());
    CY_CHECK(claimed.error().code == cy::ErrorCode::AlreadyExists);
    // The substrate's own refusal, passed through unchanged: it names the incumbent and the
    // challenger, which is what a developer needs to decide which row should own the field.
    CY_CHECK(contains(claimed.error().message, "weather.precipitation"));
    CY_CHECK(contains(claimed.error().message, "water.shoreline"));
    CY_CHECK_FALSE(system.fields().claimed());
}

CY_TEST_CASE("declaring the wetness owner external lets both rows produce, without a conflict") {
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());
    WaterSystem system(test::allocator(), test::partition());
    CY_REQUIRE(system.registry().add(flat_sea()).has_value());
    system.set_bed_source(&beach_bed, nullptr);

    WaterFieldOptions options;
    options.wetness_owner = WetnessOwner::External;
    CY_REQUIRE(system.fields().declare(registry, options).has_value());
    // Water does not declare `wetness` at all in this configuration: it is weather's field, and
    // weather declares it.
    CY_CHECK(registry.find(cy::environment::field_id(cy::environment::fields::kWetness)) ==
             nullptr);
    CY_REQUIRE(registry.declare(weather_wetness()).has_value());

    // Weather owns `wetness`; water owns the shore's contribution to it, under its own name.
    const auto weather =
        registry.claim(cy::environment::field_id(cy::environment::fields::kWetness),
                       "weather.precipitation", cy::environment::ProducerKind::System);
    CY_REQUIRE(weather.has_value());
    CY_REQUIRE(system.fields().claim(registry, store).has_value());
    CY_CHECK(system.fields().wetness_field() ==
             cy::environment::field_id(cy::water::kShoreWetnessField));
    CY_CHECK_EQ(static_cast<int>(system.fields().wetness_owner()),
                static_cast<int>(WetnessOwner::External));

    // And the shore's contribution is still published, for the wetness producer to compose.
    CY_REQUIRE(
        system.fields()
            .publish(system.shoreline_inputs(), FieldResidency::Local, -40.0, 0.0, 40.0, 16.0)
            .has_value());
    CY_CHECK_NEAR(
        store.sample(system.fields().wetness_field(), cy::world::WorldVec3d{-10.0, 0.0, 7.0})
            .value.x(),
        1.0F, 0.02F);
}

CY_TEST_CASE("publishing refuses what it cannot publish") {
    FieldRegistry registry(test::allocator());
    FieldStore store(test::allocator(), registry, test::partition());
    WaterSystem system(test::allocator(), test::partition());
    CY_REQUIRE(system.registry().add(flat_sea()).has_value());

    // Before a claim there is nothing to write through.
    CY_CHECK_FALSE(
        system.fields()
            .publish(system.shoreline_inputs(), FieldResidency::Local, 0.0, 0.0, 10.0, 10.0)
            .has_value());

    WaterFieldOptions bad;
    bad.regional_cell_metres = bad.local_cell_metres;
    CY_CHECK_FALSE(system.fields().declare(registry, bad).has_value());

    WaterFieldOptions options;
    CY_REQUIRE(system.fields().declare(registry, options).has_value());
    CY_REQUIRE(system.fields().claim(registry, store).has_value());
    // An empty rectangle, and one smaller than a cell of the level asked for.
    CY_CHECK_FALSE(
        system.fields()
            .publish(system.shoreline_inputs(), FieldResidency::Local, 10.0, 0.0, 10.0, 10.0)
            .has_value());
}
