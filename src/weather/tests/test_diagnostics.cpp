// The environment inspector: an explanation that sums to the answer, and a wind whose sources are
// separable. M10 tasks 3.1 and 3.2.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`: the inspector's
// wind breakdown was pointed at a SECOND `WindComposer::sample()` call taken at a different moment
// (`seconds() + 1`) instead of at the `explain()` of the same walk. "wind sources are separable and
// they sum to the result" went red on the equality, reporting a gust term that differed in the
// third decimal. The single walk was restored.

#include <cy/test/test.h>

#include <cy/weather/diagnostics.h>
#include <cy/weather/system.h>

#include <cmath>

#include "fixtures.h"

namespace test = cy::weather::test;
using cy::weather::CellOverlayQuad;
using cy::weather::CellOverlayQuantity;
using cy::weather::ClimateMap;
using cy::weather::EnvironmentInspection;
using cy::weather::EnvironmentInspector;
using cy::weather::RainSourceKind;
using cy::weather::SkyOcclusion;
using cy::weather::StormId;
using cy::weather::StormPathPoint;
using cy::weather::StormType;
using cy::weather::WeatherSystem;
using cy::weather::WindSourceKind;

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

CY_TEST_CASE("the inspector reports every quantity the specification lists") {
    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(test::config(), uniform_climate()).has_value());
    system.set_terrain(test::ridge());
    SkyOcclusion occlusion(test::allocator());
    CY_REQUIRE(occlusion.configure(2.0F).has_value());
    CY_REQUIRE(occlusion.add_cover(15'990.0, -5.0, 16'010.0, 5.0, 6.0F, 0.8F).has_value());
    system.set_occlusion(&occlusion);
    CY_REQUIRE(system.storms()
                   .spawn(test::storm_at(1, 16'000.0, 0.0, StormType::Thunderstorm, 0.9F))
                   .has_value());
    CY_REQUIRE(system.advance(test::at_tick(1), 900.0).has_value());

    const EnvironmentInspector inspector(system);
    const EnvironmentInspection report =
        inspector.inspect(cy::world::WorldVec3d{16'000.0, 1.6, 0.0});

    CY_CHECK_GT(report.sample.pressure_hpa, 800.0F);
    CY_CHECK_GT(report.sample.visibility_metres, 0.0F);
    CY_CHECK_GT(report.climate.rainfall_potential_mm, 0.0F);
    CY_CHECK_GT(report.potential.vegetation, 0.0F);
    CY_CHECK_GT(report.cell_metres, 0.0F);
    // The climate REGION the inspector reports is the grid cell, so a developer can find it.
    CY_CHECK_GE(report.cell_x, 0);
    // The shelter, and therefore why the ground under the arch is not getting wet.
    CY_CHECK(report.shelter.sheltered);
    CY_CHECK_NEAR(report.shelter.sky_visibility, 0.2F, 0.001F);
}

CY_TEST_CASE("wind sources are separable and they sum to the result") {
    // "WHEN wind is inspected THEN prevailing, storm, terrain, and local contributions SHALL be
    // shown SEPARATELY ALONGSIDE THE RESULT."
    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(test::config(), uniform_climate()).has_value());
    system.set_terrain(test::ridge());
    CY_REQUIRE(system.storms()
                   .spawn(test::storm_at(2, 16'000.0, 0.0, StormType::Cyclone, 0.7F))
                   .has_value());
    cy::weather::WindVolume duct;
    duct.kind = cy::weather::WindVolumeKind::Directional;
    duct.position = cy::world::WorldVec3d{16'000.0, 0.0, 0.0};
    duct.radius_metres = 120.0F;
    duct.strength = 15.0F;
    duct.direction = cy::Vec3{0.0F, 0.0F, 1.0F};
    CY_REQUIRE(system.wind().add_volume(duct).has_value());
    CY_REQUIRE(system.advance(test::at_tick(4), 900.0).has_value());

    const EnvironmentInspector inspector(system);
    const EnvironmentInspection report =
        inspector.inspect(cy::world::WorldVec3d{16'020.0, 8.0, 10.0});
    CY_REQUIRE((report.wind.count) > (4u));

    bool prevailing = false;
    bool storm = false;
    bool terrain = false;
    bool volume = false;
    bool gust = false;
    for (cy::u32 index = 0; index < report.wind.count; ++index) {
        switch (report.wind.sources[index].kind) {
            case WindSourceKind::Prevailing:
                prevailing = true;
                break;
            case WindSourceKind::Storm:
                storm = true;
                break;
            case WindSourceKind::Terrain:
                terrain = true;
                break;
            case WindSourceKind::Volume:
                volume = true;
                break;
            case WindSourceKind::Gust:
                gust = true;
                break;
            default:
                break;
        }
    }
    CY_CHECK(prevailing);
    CY_CHECK(storm);
    CY_CHECK(terrain);
    CY_CHECK(volume);
    CY_CHECK(gust);

    // THE SUM IS THE ANSWER, exactly. An inspector that re-derived the terms could explain a wind
    // the simulation did not produce; this one cannot, because it is the same walk.
    CY_CHECK_EQ(report.wind.total.x, report.wind.sample.full().x);
    CY_CHECK_EQ(report.wind.total.y, report.wind.sample.full().y);
    CY_CHECK_EQ(report.wind.total.z, report.wind.sample.full().z);
}

CY_TEST_CASE("why is it raining here: a storm, a slope, and a remainder") {
    // "It SHALL EXPLAIN COMPOSITION: a rain rate reported as a storm's contribution, a terrain rain
    // shadow reduction, and a local volume's addition, RATHER THAN AS ONE NUMBER."
    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(test::config(), uniform_climate()).has_value());
    system.set_terrain(test::ridge());
    // A STATIONARY storm: the claim under test is the composition at a position, and a storm that
    // walked out of the probe during the hour would make the test measure its velocity instead.
    cy::weather::Storm parked = test::storm_at(3, 14'000.0, 0.0, StormType::RainBand, 0.8F);
    parked.velocity = cy::Vec2{0.0F, 0.0F};
    CY_REQUIRE(system.storms().spawn(parked).has_value());
    CY_REQUIRE(system.advance(test::at_tick(6), 1'800.0).has_value());

    const EnvironmentInspector inspector(system);

    // WINDWARD: the storm is named, and the orographic gain is reported as a gain.
    const EnvironmentInspection wet = inspector.inspect(cy::world::WorldVec3d{14'000.0, 2.0, 0.0});
    bool named_storm = false;
    bool reported_gain = false;
    for (cy::u32 index = 0; index < wet.precipitation.count; ++index) {
        named_storm =
            named_storm || (wet.precipitation.sources[index].kind == RainSourceKind::Storm &&
                            wet.precipitation.sources[index].storm == StormId{3});
        reported_gain = reported_gain ||
                        wet.precipitation.sources[index].kind == RainSourceKind::OrographicGain;
    }
    CY_CHECK(named_storm);
    CY_CHECK(reported_gain);
    CY_CHECK_GT(wet.precipitation.uplift_mps, 0.0F);
    CY_CHECK_NEAR(wet.precipitation.total_mm_per_hour, wet.sample.precipitation_mm_per_hour,
                  0.001F);

    // LEE: the shadow is reported, and it is reported NEGATIVE. A "shadow" with a positive sign
    // would invert what a designer reads.
    const EnvironmentInspection dry = inspector.inspect(cy::world::WorldVec3d{26'000.0, 2.0, 0.0});
    bool reported_shadow = false;
    for (cy::u32 index = 0; index < dry.precipitation.count; ++index) {
        if (dry.precipitation.sources[index].kind == RainSourceKind::RainShadow) {
            reported_shadow = true;
            CY_CHECK_LE(dry.precipitation.sources[index].mm_per_hour, 0.0F);
        }
    }
    CY_CHECK(reported_shadow);
    CY_CHECK_LT(dry.precipitation.uplift_mps, 0.0F);
}

CY_TEST_CASE("weather cells and storm paths come out as data a view can draw") {
    WeatherSystem system(test::allocator());
    CY_REQUIRE(system.configure(test::config(), uniform_climate()).has_value());
    cy::weather::Storm storm = test::storm_at(9, 0.0, 0.0, StormType::Squall, 0.6F);
    storm.velocity = cy::Vec2{30.0F, 0.0F};
    CY_REQUIRE(system.storms().spawn(storm).has_value());
    for (cy::u32 tick = 1; tick <= 10; ++tick) {
        CY_REQUIRE(system.advance(test::at_tick(tick), 60.0).has_value());
    }

    const EnvironmentInspector inspector(system);
    cy::Array<StormPathPoint> path(test::allocator());
    CY_REQUIRE(inspector.storm_path(StormId{9}, 6, 60.0F, path).has_value());
    CY_REQUIRE((path.size()) > (1u));
    // The track runs backwards from where the storm is now — reconstructed, as diagnostics.h says,
    // not recorded.
    CY_CHECK_GT(path[0].position.x, path[path.size() - 1].position.x);
    CY_CHECK_FALSE(inspector.storm_path(StormId{404}, 4, 60.0F, path).has_value());

    cy::Array<CellOverlayQuad> quads(test::allocator());
    CY_REQUIRE(
        inspector
            .cell_overlay(CellOverlayQuantity::Precipitation, 0.0, 0.0, 20'000.0, 4'000.0, quads)
            .has_value());
    CY_REQUIRE((quads.size()) > (2u));
    for (const CellOverlayQuad& quad : quads.span()) {
        CY_CHECK_GT(quad.max_x, quad.min_x);
        CY_CHECK_GT(quad.max_z, quad.min_z);
        CY_CHECK_GE(quad.value, 0.0F);
    }
}

CY_TEST_CASE("an unbound inspector answers rather than faulting") {
    const EnvironmentInspector inspector;
    CY_CHECK_FALSE(inspector.bound());
    const EnvironmentInspection empty = inspector.inspect(cy::world::WorldVec3d{1.0, 2.0, 3.0});
    CY_CHECK_EQ(empty.wind.count, 0u);
    cy::Array<CellOverlayQuad> quads(test::allocator());
    CY_CHECK_FALSE(
        inspector.cell_overlay(CellOverlayQuantity::Temperature, 0.0, 0.0, 1.0, 1.0, quads)
            .has_value());
}
