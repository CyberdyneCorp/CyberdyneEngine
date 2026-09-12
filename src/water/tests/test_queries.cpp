// The water query, buoyancy on the authoritative surface, and the character states derived from
// depth. M10 task 2.3, and `water`'s "Water queries", "Buoyancy and physics interaction" and
// "Water navigation" requirements.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`: `sample_body()`'s
// spectral branch was changed to `BandSelection::All`, which is the classic bug's mirror image —
// physics floating on the rendered surface. "a boat floats on the waves it appears to float on, and
// not on a flat plane" went red, because the queried height then matched the rendered one exactly
// and the case requires the visual band to be excluded. It was restored.

#include <cy/test/test.h>

#include <cy/water/system.h>

#include <algorithm>
#include <cmath>

#include "fixtures.h"

namespace test = cy::water::test;
using cy::water::BuoyancyParams;
using cy::water::BuoyancySample;
using cy::water::BuoyancyState;
using cy::water::CharacterWaterState;
using cy::water::SwimThresholds;
using cy::water::WaterResolution;
using cy::water::WaterSample;
using cy::water::WaterSystem;

namespace {

/// A flat bed twenty metres down, so a query has a column to measure.
cy::f64 deep_bed(void*, cy::f64, cy::f64) noexcept {
    return -20.0;
}

/// A beach: the ground rises one metre for every ten metres of x, crossing sea level at x = 0.
cy::f64 beach_bed(void*, cy::f64 x, cy::f64) noexcept {
    return x * 0.1;
}

}  // namespace

CY_TEST_CASE("one query answers every backend, and reports how it was answered") {
    WaterSystem system(test::allocator(), test::partition());
    const auto ocean = *system.registry().add(test::ocean_desc());
    const auto pool = *system.registry().add(test::pool_desc());
    CY_REQUIRE(system.set_ocean(ocean, test::rough_sea(), 42).has_value());
    system.set_bed_source(&deep_bed, nullptr);

    const WaterSample at_sea = system.query(cy::world::WorldVec3d{-100.0, -1.0, 40.0});
    CY_CHECK(at_sea.body == ocean);
    CY_CHECK_EQ(static_cast<int>(at_sea.resolution), static_cast<int>(WaterResolution::Simulated));
    CY_CHECK_NEAR(at_sea.density, 1025.0F, 1e-3F);
    CY_CHECK_GT(at_sea.column_thickness, 19.0F);
    CY_CHECK_GT(at_sea.submersion, 0.0F);
    CY_CHECK(at_sea.in_water());
    // Every member of the specification's own list is filled: a normal, a velocity, a depth to bed,
    // a column thickness, a density and an identity.
    CY_CHECK_NEAR(cy::length(at_sea.normal), 1.0F, 1e-4F);
    CY_CHECK_NEAR(at_sea.depth_to_bed, 19.0F, 1e-3F);

    // The flat pool is the same call and the same structure — no consumer learns which backend it
    // is holding.
    const WaterSample in_pool = system.query(cy::world::WorldVec3d{2005.0, 29.0, 2004.0});
    CY_CHECK(in_pool.body == pool);
    CY_CHECK_EQ(static_cast<int>(in_pool.resolution), static_cast<int>(WaterResolution::Simulated));
    CY_CHECK_NEAR(static_cast<cy::f32>(in_pool.surface_height), 30.0F, 1e-4F);

    // And a position in no body at all is answered rather than faulted.
    const WaterSample dry = system.query(cy::world::WorldVec3d{50'000.0, 0.0, 50'000.0});
    CY_CHECK_EQ(static_cast<int>(dry.resolution), static_cast<int>(WaterResolution::None));
    CY_CHECK_FALSE(dry.body.is_valid());
    CY_CHECK_FALSE(dry.in_water());
}

CY_TEST_CASE("a boat floats on the waves it appears to float on, and not on a flat plane") {
    WaterSystem system(test::allocator(), test::partition());
    const auto ocean = *system.registry().add(test::ocean_desc());
    cy::water::OceanParams rough = test::rough_sea();
    rough.wind_speed_mps = 14.0F;
    CY_REQUIRE(system.set_ocean(ocean, rough, 2024).has_value());
    system.set_bed_source(&deep_bed, nullptr);
    system.set_time(11.5);

    const cy::water::DisplacementModel* model = system.model_of(ocean);
    CY_REQUIRE(model != nullptr);

    cy::f64 largest_swell = 0.0;
    for (cy::u32 step = 0; step < 32; ++step) {
        const cy::f64 x = static_cast<cy::f64>(step) * 7.0;
        const WaterSample sample = system.query(cy::world::WorldVec3d{x, 0.0, 0.0});
        const cy::water::Displacement authoritative =
            evaluate_displacement(*model, cy::water::BandSelection::Authoritative, x, 0.0, 11.5);
        const cy::water::Displacement rendered =
            evaluate_displacement(*model, cy::water::BandSelection::All, x, 0.0, 11.5);

        // THE CONTRACT: what the query answers IS the authoritative evaluation, to the bit.
        CY_CHECK_EQ(sample.surface_height, authoritative.height);
        // And it is not the flat plane the classic bug puts a boat on.
        largest_swell = std::max(largest_swell, std::fabs(sample.surface_height));
        // The rendered surface may differ, and only by the declared visual bands.
        CY_CHECK_LE(std::fabs(rendered.height - sample.surface_height), 0.06);
    }
    CY_CHECK_GT(largest_swell, 0.5);
}

CY_TEST_CASE("a query outside resident data returns the body's mean level, and says so") {
    WaterSystem system(test::allocator(), test::partition());
    const auto ocean = *system.registry().add(test::ocean_desc());
    CY_REQUIRE(system.set_ocean(ocean, test::rough_sea(), 5).has_value());
    system.set_bed_source(&deep_bed, nullptr);

    // A world with a streamer and one bound cell near the origin. Everything outside it is a region
    // whose water data is not resident.
    cy::world::CellEventQueue events(test::allocator());
    CY_REQUIRE(system.streaming().attach(events, 40).has_value());
    const cy::world::CellCoord coord{0, 0, 0, 0};
    const cy::world::CellId cell = cy::world::cell_id_of(test::partition(), coord);
    CY_REQUIRE(system.streaming().declare_cell(cell, coord).has_value());
    cy::world::CellEvent event;
    event.kind = cy::world::CellEventKind::Resident;
    event.cell = cell;
    event.channels = cy::world::ChannelMask::all();
    CY_REQUIRE(events.emit(event).has_value());
    CY_REQUIRE(system.streaming().tick().has_value());
    CY_CHECK_GT(system.streaming().resident_segments(), 0u);

    const WaterSample near = system.query(cy::world::WorldVec3d{40.0, -1.0, 40.0});
    CY_CHECK_EQ(static_cast<int>(near.resolution), static_cast<int>(WaterResolution::Simulated));

    const WaterSample distant = system.query(cy::world::WorldVec3d{3000.0, -1.0, 3000.0});
    CY_CHECK_EQ(static_cast<int>(distant.resolution), static_cast<int>(WaterResolution::MeanLevel));
    CY_CHECK(distant.body == ocean);
    // The body's mean level, exactly — not a stale wave and not a zero.
    CY_CHECK_NEAR(static_cast<cy::f32>(distant.surface_height), 0.0F, 1e-6F);
    CY_CHECK_GT(system.diagnostics().mean_level_queries, 0u);
}

CY_TEST_CASE("a hull is sampled at forty points in one batched call") {
    WaterSystem system(test::allocator(), test::partition());
    const auto ocean = *system.registry().add(test::ocean_desc());
    CY_REQUIRE(system.set_ocean(ocean, test::rough_sea(), 8).has_value());
    system.set_bed_source(&deep_bed, nullptr);

    cy::world::WorldVec3d positions[40];
    WaterSample samples[40];
    for (cy::u32 index = 0; index < 40; ++index) {
        positions[index] =
            cy::world::WorldVec3d{static_cast<cy::f64>(index) * 0.5 - 10.0, -0.2, 3.0};
    }
    CY_REQUIRE(system
                   .query_many(cy::Span<const cy::world::WorldVec3d>(positions, 40),
                               cy::Span<WaterSample>(samples, 40))
                   .has_value());
    for (cy::u32 index = 0; index < 40; ++index) {
        CY_CHECK(samples[index].body == ocean);
        // Each is the authoritative answer at its own position, not the first one repeated.
        CY_CHECK_EQ(samples[index].surface_height, system.query(positions[index]).surface_height);
    }
    // A mismatched batch is refused rather than truncated.
    CY_CHECK_FALSE(system
                       .query_many(cy::Span<const cy::world::WorldVec3d>(positions, 40),
                                   cy::Span<WaterSample>(samples, 20))
                       .has_value());
}

CY_TEST_CASE("a long vessel pitches with the swell rather than translating rigidly") {
    WaterSystem system(test::allocator(), test::partition());
    const auto ocean = *system.registry().add(test::ocean_desc());
    // One clean swell of a known wavelength, so "longer than the wavelength" is a fact rather than
    // a hope: 40 m waves, and a 60 m hull across them.
    cy::water::OceanParams swell = test::rough_sea();
    swell.shortest_wavelength = 30.0F;
    swell.longest_wavelength = 60.0F;
    swell.cascades = 1;
    swell.trains_per_cascade = 1;
    swell.spread_degrees = 0.0F;
    swell.wind_speed_mps = 12.0F;
    CY_REQUIRE(system.set_ocean(ocean, swell, 314).has_value());
    system.set_bed_source(&deep_bed, nullptr);

    BuoyancySample hull[2];
    hull[0].offset = cy::Vec3{-30.0F, 0.0F, 0.0F};
    hull[0].volume = 4.0F;
    hull[1].offset = cy::Vec3{30.0F, 0.0F, 0.0F};
    hull[1].volume = 4.0F;
    BuoyancyState state;
    state.position = cy::world::WorldVec3d{0.0, -0.5, 0.0};
    BuoyancyParams params;

    // Over a wave period the bow and the stern are at different heights, so the torque is non-zero
    // somewhere — which a single centre sample could not produce at all.
    cy::f32 largest_torque = 0.0F;
    for (cy::u32 step = 0; step < 24; ++step) {
        system.set_time(static_cast<cy::f64>(step) * 0.4);
        const auto result = system.buoyancy(state, cy::Span<const BuoyancySample>(hull, 2), params);
        CY_REQUIRE(result.has_value());
        largest_torque = std::max(largest_torque, std::fabs(result->torque.z));
    }
    CY_CHECK_GT(largest_torque, 1000.0F);

    // The same hull sampled at ONE centre point produces no torque at all, whatever the sea does.
    BuoyancySample centre[1];
    centre[0].offset = cy::Vec3{0.0F, 0.0F, 0.0F};
    centre[0].volume = 8.0F;
    cy::f32 centre_torque = 0.0F;
    for (cy::u32 step = 0; step < 24; ++step) {
        system.set_time(static_cast<cy::f64>(step) * 0.4);
        const auto result =
            system.buoyancy(state, cy::Span<const BuoyancySample>(centre, 1), params);
        CY_REQUIRE(result.has_value());
        centre_torque = std::max(centre_torque, cy::length(result->torque));
    }
    CY_CHECK_NEAR(centre_torque, 0.0F, 1e-3F);
}

CY_TEST_CASE("buoyancy lifts against gravity, and only what is submerged") {
    WaterSystem system(test::allocator(), test::partition());
    const auto pool = *system.registry().add(test::pool_desc());
    (void)pool;
    system.set_bed_source(nullptr, nullptr);

    BuoyancySample hull[1];
    hull[0].offset = cy::Vec3{0.0F, 0.0F, 0.0F};
    hull[0].volume = 2.0F;
    BuoyancyParams params;

    BuoyancyState under;
    under.position = cy::world::WorldVec3d{2005.0, 29.0, 2004.0};
    const auto submerged = system.buoyancy(under, cy::Span<const BuoyancySample>(hull, 1), params);
    CY_REQUIRE(submerged.has_value());
    // rho g V, upward: 1000 x 9.80665 x 2 = 19 613 N.
    // A relative epsilon of 1e-4 against 19 613 N is two newtons; `CY_CHECK_NEAR`'s third argument
    // is relative, so a tolerance of 1.0F here would accept any number at all.
    CY_CHECK_NEAR(submerged->displacement_force.y, 1000.0F * cy::water::kGravity * 2.0F, 1e-4F);
    CY_CHECK_EQ(submerged->submerged_samples, 1u);

    BuoyancyState above;
    above.position = cy::world::WorldVec3d{2005.0, 30.6, 2004.0};
    const auto dry = system.buoyancy(above, cy::Span<const BuoyancySample>(hull, 1), params);
    CY_REQUIRE(dry.has_value());
    CY_CHECK_EQ(dry->submerged_samples, 0u);
    CY_CHECK_NEAR(cy::length(dry->force), 0.0F, 1e-4F);

    CY_CHECK_EQ(system.diagnostics().buoyancy_solves, 2u);
    CY_CHECK_EQ(system.diagnostics().buoyancy_samples, 2u);
}

CY_TEST_CASE("a current carries a floating object downstream") {
    WaterSystem system(test::allocator(), test::partition());
    const auto river = *system.registry().add(test::river_desc());
    (void)river;
    CY_REQUIRE(test::author_straight_river(system.rivers(), 10.0F).has_value());
    cy::water::RiverBuildReport report;
    CY_REQUIRE(system.rivers().build(report).has_value());

    const WaterSample sample = system.query(cy::world::WorldVec3d{50.0, 8.5, 100.0});
    CY_REQUIRE(sample.in_water());
    CY_CHECK_GT(sample.velocity.x, 0.5F);

    BuoyancySample debris[1];
    debris[0].offset = cy::Vec3{0.0F, 0.0F, 0.0F};
    debris[0].volume = 0.05F;
    BuoyancyState state;
    state.position = cy::world::WorldVec3d{50.0, 8.6, 100.0};
    BuoyancyParams params;

    const auto result = system.buoyancy(state, cy::Span<const BuoyancySample>(debris, 1), params);
    CY_REQUIRE(result.has_value());
    // Stationary debris in a moving river is pushed DOWNSTREAM: the drag term is computed against
    // the water's velocity, which is the same flow field a particle and the AI read.
    CY_CHECK_GT(result->force.x, 0.0F);
    CY_CHECK_GT(result->water_velocity.x, 0.5F);
}

CY_TEST_CASE("wading and swimming are derived from the depth at the character's position") {
    const SwimThresholds thresholds;
    CY_CHECK_EQ(static_cast<int>(cy::water::character_state(0.1F, thresholds)),
                static_cast<int>(CharacterWaterState::Dry));
    CY_CHECK_EQ(static_cast<int>(cy::water::character_state(0.8F, thresholds)),
                static_cast<int>(CharacterWaterState::Wading));
    CY_CHECK_EQ(static_cast<int>(cy::water::character_state(2.0F, thresholds)),
                static_cast<int>(CharacterWaterState::Swimming));

    // And through the system, against a beach: a character walks into the sea and the state changes
    // with the ground under them rather than with where the surface is.
    WaterSystem system(test::allocator(), test::partition());
    const auto ocean = *system.registry().add(test::ocean_desc());
    (void)ocean;
    system.set_bed_source(&beach_bed, nullptr);

    CY_CHECK_EQ(static_cast<int>(
                    system.character_state_at(cy::world::WorldVec3d{20.0, 2.0, 0.0}, thresholds)),
                static_cast<int>(CharacterWaterState::Dry));
    CY_CHECK_EQ(static_cast<int>(
                    system.character_state_at(cy::world::WorldVec3d{-8.0, 0.0, 0.0}, thresholds)),
                static_cast<int>(CharacterWaterState::Wading));
    CY_CHECK_EQ(static_cast<int>(
                    system.character_state_at(cy::world::WorldVec3d{-40.0, 0.0, 0.0}, thresholds)),
                static_cast<int>(CharacterWaterState::Swimming));
}

CY_TEST_CASE("downstream is cheaper than upstream, through the flow field") {
    WaterSystem system(test::allocator(), test::partition());
    CY_REQUIRE(system.registry().add(test::river_desc()).has_value());
    CY_REQUIRE(test::author_straight_river(system.rivers(), 10.0F).has_value());
    cy::water::RiverBuildReport report;
    CY_REQUIRE(system.rivers().build(report).has_value());

    const cy::water::WaterNavigationCosts costs;
    const cy::world::WorldVec3d upstream_end{40.0, 8.5, 100.0};
    const cy::world::WorldVec3d downstream_end{60.0, 8.5, 100.0};

    const cy::f32 downstream = system.directional_cost(upstream_end, downstream_end, costs);
    const cy::f32 upstream = system.directional_cost(downstream_end, upstream_end, costs);
    CY_CHECK_LT(downstream, upstream);
    // And neither is free, however helpful the current: a zero-cost edge breaks a path search.
    CY_CHECK_GE(downstream, costs.minimum_multiplier);

    // Across the flow, the two directions cost the same — the assist is the component ALONG the
    // travel, and a cost that fell for any movement in flowing water would pass the case above.
    const cy::f32 across = system.directional_cost(cy::world::WorldVec3d{50.0, 8.5, 98.0},
                                                   cy::world::WorldVec3d{50.0, 8.5, 102.0}, costs);
    const cy::f32 back = system.directional_cost(cy::world::WorldVec3d{50.0, 8.5, 102.0},
                                                 cy::world::WorldVec3d{50.0, 8.5, 98.0}, costs);
    CY_CHECK_NEAR(across, back, 1e-4F);

    // Dry ground costs one: water contributes to navigation where there is water and nowhere else.
    CY_CHECK_NEAR(system.directional_cost(cy::world::WorldVec3d{50.0, 8.5, 300.0},
                                          cy::world::WorldVec3d{60.0, 8.5, 300.0}, costs),
                  1.0F, 1e-4F);
}

CY_TEST_CASE("a flood emits a navigation dirty region, once, with its own delta") {
    WaterSystem system(test::allocator(), test::partition());
    const auto lake = *system.registry().add(test::lake_desc());

    cy::Array<cy::water::WaterDirtyRegion> regions(test::allocator());
    CY_REQUIRE(system.drain_navigation_dirty(regions).has_value());
    CY_CHECK_EQ(regions.size(), 0u);

    CY_REQUIRE(system.set_mean_level(lake, 13.5).has_value());
    CY_REQUIRE(system.drain_navigation_dirty(regions).has_value());
    CY_REQUIRE_EQ(regions.size(), 1u);
    CY_CHECK(regions[0].body == lake);
    CY_CHECK_NEAR(regions[0].level_delta, 1.5F, 1e-4F);
    CY_CHECK_NEAR(static_cast<cy::f32>(regions[0].min_x), 500.0F, 1e-3F);

    // Drained means drained: a consumer that drains twice must not rebuild twice.
    regions.clear();
    CY_REQUIRE(system.drain_navigation_dirty(regions).has_value());
    CY_CHECK_EQ(regions.size(), 0u);

    // And the level actually moved: the surface a query answers with has risen.
    const WaterSample sample = system.query(cy::world::WorldVec3d{700.0, 13.0, 700.0});
    CY_CHECK_NEAR(static_cast<cy::f32>(sample.surface_height), 13.5F, 1e-3F);
}

CY_TEST_CASE("the point report names the body, the height and the bands that made it") {
    WaterSystem system(test::allocator(), test::partition());
    const auto ocean = *system.registry().add(test::ocean_desc());
    CY_REQUIRE(system.registry().add(test::river_desc()).has_value());
    CY_REQUIRE(system.set_ocean(ocean, test::rough_sea(), 77).has_value());
    system.set_bed_source(&deep_bed, nullptr);

    cy::water::WaterPointReport report;
    cy::water::BandContribution bands[cy::water::kMaxDisplacementBands];
    CY_REQUIRE(
        system.point_report(cy::world::WorldVec3d{200.0, 0.0, 150.0}, report, bands).has_value());

    CY_CHECK(report.sample.body == ocean);
    CY_CHECK_EQ(static_cast<int>(report.backend),
                static_cast<int>(cy::water::WaterBackend::Spectral));
    CY_CHECK_EQ(report.band_count, 4u);
    CY_CHECK_GT(report.authoritative_metres, 0.0F);
    // The overlap is in the report: this position is inside the river's bounds as well as the
    // sea's, and a developer asking "which body owns it" should be able to see both.
    CY_CHECK_EQ(report.overlapping_bodies, 2u);

    // "which bands contributed" — the authoritative ones sum to the queried surface's own offset.
    cy::f32 authoritative = 0.0F;
    for (cy::u32 index = 0; index < report.band_count; ++index) {
        if (bands[index].authority == cy::water::BandAuthority::Authoritative) {
            authoritative += bands[index].vertical_metres;
        }
    }
    CY_CHECK_NEAR(static_cast<cy::f32>(report.sample.surface_height), authoritative, 1e-3F);
}

CY_TEST_CASE("a river answers inside its channel and the sea answers on the bank") {
    WaterSystem system(test::allocator(), test::partition());
    const auto ocean = *system.registry().add(test::ocean_desc());
    const auto river = *system.registry().add(test::river_desc());
    CY_REQUIRE(system.set_ocean(ocean, test::rough_sea(), 6).has_value());
    CY_REQUIRE(test::author_straight_river(system.rivers(), 10.0F).has_value());
    cy::water::RiverBuildReport report;
    CY_REQUIRE(system.rivers().build(report).has_value());
    system.set_bed_source(&deep_bed, nullptr);

    // In the channel, the higher-priority river answers.
    const WaterSample in_channel = system.query(cy::world::WorldVec3d{150.0, 8.0, 100.0});
    CY_CHECK(in_channel.body == river);
    CY_CHECK_GT(in_channel.velocity.x, 0.0F);

    // On the bank — inside the river body's BOUNDS but outside its channel — the sea answers,
    // because the river genuinely has no water there. One consistent answer, and a useful one.
    const WaterSample on_bank = system.query(cy::world::WorldVec3d{150.0, 0.0, 200.0});
    CY_CHECK(on_bank.body == ocean);
}
