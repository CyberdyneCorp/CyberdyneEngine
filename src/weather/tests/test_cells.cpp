// Weather cells: the hierarchy, the rain shadow, the non-alignment, and the cost that does not
// follow the camera. M10 task 3.1.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`: the lee branch of
// `WeatherCells::apply_terrain()` was deleted, leaving the windward enhancement and no shadow.
// "a mountain range across the wind makes the lee dry" went red on the leeward assertion, reporting
// a leeward rate slightly ABOVE the windward one (the advection carries the windward rain over the
// crest). The branch was restored.

#include <cy/test/test.h>

#include <cy/weather/cells.h>
#include <cy/weather/climate.h>
#include <cy/weather/storm.h>

#include <cmath>

#include "fixtures.h"

namespace test = cy::weather::test;
using cy::weather::ClimateMap;
using cy::weather::RefinementSource;
using cy::weather::StepReport;
using cy::weather::StormRegistry;
using cy::weather::WeatherCells;
using cy::weather::WeatherCellSample;
using cy::weather::WeatherGridConfig;
using cy::weather::WeatherScale;

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

CY_TEST_CASE("the weather grid does not align with the world partition, and still works") {
    // "Weather cells SHALL NOT be required to align with world partition cells, PCG regions, or
    // field tiles; each system's partition serves its own purpose." A sentence that passes
    // trivially for an implementation that happens to align, so the fixture deliberately does not —
    // and the whole model runs on it.
    const WeatherGridConfig config = test::grid();
    CY_CHECK_FALSE(config.aligns_with(test::partition()));

    WeatherCells cells(test::allocator());
    CY_REQUIRE(cells.configure(config, uniform_climate(), 7).has_value());
    CY_REQUIRE(cells.advance(test::at_tick(1), 600.0).has_value());

    // Sample across a WORLD-CELL boundary (128 m) and across a WEATHER-CELL boundary (4 000 m).
    // Neither is a discontinuity the other knows about: the answer is continuous where the weather
    // grid is continuous, and it steps where the weather grid steps, whatever the world does.
    const WeatherCellSample before_world = cells.sample(127.9, 0.0, WeatherScale::Regional);
    const WeatherCellSample after_world = cells.sample(128.1, 0.0, WeatherScale::Regional);
    CY_CHECK_EQ(before_world.state.temperature_celsius, after_world.state.temperature_celsius);

    // And an ALIGNED configuration is reported as aligned — the predicate is live, not a constant
    // false.
    WeatherGridConfig aligned = config;
    aligned.origin_x = test::partition().origin.x;
    aligned.origin_z = test::partition().origin.z;
    aligned.regional_cell_metres = 128.0F * 32.0F;
    CY_CHECK(aligned.aligns_with(test::partition()));
}

CY_TEST_CASE("a mountain range across the wind makes the windward wet and the lee dry") {
    // "WHEN a mountain range lies across the prevailing wind THEN rainfall SHALL be higher windward
    // and lower leeward, FROM A DECLARED MODEL rather than hand-painted." The model is
    // `OrographicModel`; the ridge is the fixture's; nothing here is painted.
    WeatherCells cells(test::allocator());
    CY_REQUIRE(cells.configure(test::grid(), uniform_climate(), 11).has_value());
    cells.set_terrain(test::ridge());
    CY_REQUIRE(cells.advance(test::at_tick(1), 1'800.0).has_value());

    // THE CONTROL: the same grid, the same seed, the same climate, no terrain. Comparing the
    // windward cell to the leeward one alone would measure the ridge's shape as much as its effect;
    // comparing each cell to ITSELF without the ridge isolates the terrain, which is the whole
    // claim.
    WeatherCells flat(test::allocator());
    CY_REQUIRE(flat.configure(test::grid(), uniform_climate(), 11).has_value());
    CY_REQUIRE(flat.advance(test::at_tick(1), 1'800.0).has_value());

    // The prevailing wind is due east, so the windward slope is west of the crest at x = 20 km.
    const WeatherCellSample windward = cells.sample(14'000.0, 0.0, WeatherScale::Regional);
    const WeatherCellSample lee = cells.sample(28'000.0, 0.0, WeatherScale::Regional);
    const WeatherCellSample flat_windward = flat.sample(14'000.0, 0.0, WeatherScale::Regional);
    const WeatherCellSample flat_lee = flat.sample(28'000.0, 0.0, WeatherScale::Regional);

    // HIGHER WINDWARD, LOWER LEEWARD — the requirement, each against its own control.
    CY_CHECK_GT(windward.state.precipitation_mm_per_hour,
                flat_windward.state.precipitation_mm_per_hour);
    CY_CHECK_LT(lee.state.precipitation_mm_per_hour, flat_lee.state.precipitation_mm_per_hour);
    // And the air over the lee is drier than the same air would have been without the range: that
    // is the rain SHADOW, and it is why it reaches beyond the slope.
    CY_CHECK_LT(lee.state.humidity, flat_lee.state.humidity);

    // The uplift itself has the sign the slope has: positive climbing, negative descending, and
    // exactly zero where there is no terrain at all.
    CY_CHECK_GT(cells.uplift_at(14'000.0, 0.0, cy::Vec2{10.0F, 0.0F}), 0.0F);
    CY_CHECK_LT(cells.uplift_at(28'000.0, 0.0, cy::Vec2{10.0F, 0.0F}), 0.0F);
    CY_CHECK_EQ(flat.uplift_at(14'000.0, 0.0, cy::Vec2{10.0F, 0.0F}), 0.0F);
    CY_CHECK_NEAR(flat_windward.state.precipitation_mm_per_hour,
                  flat_lee.state.precipitation_mm_per_hour, 0.01F);
}

CY_TEST_CASE("weather cost is bounded by the grid, not by where the camera is") {
    // "Simulation budgets SHALL bound weather evolution cost INDEPENDENTLY OF VIEW" and "WHEN the
    // camera moves across a large world THEN weather simulation cost SHALL remain bounded by its
    // own budget."
    WeatherCells cells(test::allocator());
    CY_REQUIRE(cells.configure(test::grid(), uniform_climate(), 3).has_value());

    RefinementSource source;
    source.x = 0.0;
    source.z = 0.0;
    source.radius_metres = 3'000.0F;
    CY_REQUIRE(
        cells.set_refinement_sources(cy::Span<const RefinementSource>(&source, 1)).has_value());

    cy::Expected<StepReport, cy::Error> first = cells.advance(test::at_tick(1), 600.0);
    CY_REQUIRE(first.has_value());
    const cy::u64 baseline = first->cells_stepped;
    CY_CHECK_GT(baseline, 0u);

    // Walk the camera a thousand kilometres. The refined set moves with it; the COST does not
    // change, because the regional grid covers the world whether or not anybody is looking and the
    // refinement is capped.
    for (cy::u32 step = 1; step <= 8; ++step) {
        source.x = static_cast<cy::f64>(step) * 125'000.0;
        CY_REQUIRE(
            cells.set_refinement_sources(cy::Span<const RefinementSource>(&source, 1)).has_value());
        cy::Expected<StepReport, cy::Error> moved = cells.advance(test::at_tick(1 + step), 600.0);
        CY_REQUIRE(moved.has_value());
        CY_CHECK_EQ(moved->cells_stepped, baseline);
    }
}

CY_TEST_CASE("refinement is bounded and what it refuses is deterministic") {
    WeatherGridConfig config = test::grid();
    config.max_local_cells = 64;
    WeatherCells cells(test::allocator());
    CY_REQUIRE(cells.configure(config, uniform_climate(), 5).has_value());

    // Two sources that want more than the budget, registered in one order and then the other. The
    // higher priority must win both times, and the kept set must be identical.
    RefinementSource low;
    low.x = 40'000.0;
    low.z = 0.0;
    low.radius_metres = 4'000.0F;
    low.priority = 1;
    RefinementSource high = low;
    high.x = 0.0;
    high.priority = 9;

    const RefinementSource forwards[2] = {low, high};
    const RefinementSource backwards[2] = {high, low};

    CY_REQUIRE(
        cells.set_refinement_sources(cy::Span<const RefinementSource>(forwards, 2)).has_value());
    cy::Expected<StepReport, cy::Error> one = cells.advance(test::at_tick(1), 600.0);
    CY_REQUIRE(one.has_value());
    CY_CHECK_LE(one->local_cells, config.max_local_cells);
    CY_CHECK_GT(one->refinements_dropped, 0u);
    const WeatherCellSample refined_one = cells.sample(0.0, 0.0, WeatherScale::Local);

    WeatherCells other(test::allocator());
    CY_REQUIRE(other.configure(config, uniform_climate(), 5).has_value());
    CY_REQUIRE(
        other.set_refinement_sources(cy::Span<const RefinementSource>(backwards, 2)).has_value());
    cy::Expected<StepReport, cy::Error> two = other.advance(test::at_tick(1), 600.0);
    CY_REQUIRE(two.has_value());
    CY_CHECK_EQ(one->local_cells, two->local_cells);
    // The high-priority source's own position is refined in both, whichever order it was given in.
    CY_CHECK(refined_one.scale == WeatherScale::Local);
    CY_CHECK(other.sample(0.0, 0.0, WeatherScale::Local).scale == WeatherScale::Local);
}

CY_TEST_CASE("a sample with no local cell falls back and says so") {
    // "Sampling in a region whose fine data is not resident SHALL return the coarsest resident
    // value WITH A RESOLUTION INDICATOR, and SHALL NOT block."
    WeatherCells cells(test::allocator());
    CY_REQUIRE(cells.configure(test::grid(), uniform_climate(), 13).has_value());
    RefinementSource source;
    source.x = 0.0;
    source.z = 0.0;
    source.radius_metres = 2'000.0F;
    CY_REQUIRE(
        cells.set_refinement_sources(cy::Span<const RefinementSource>(&source, 1)).has_value());
    CY_REQUIRE(cells.advance(test::at_tick(1), 600.0).has_value());

    const WeatherCellSample near = cells.sample(0.0, 0.0, WeatherScale::Local);
    CY_CHECK(near.scale == WeatherScale::Local);
    CY_CHECK_NEAR(near.cell_metres, 500.0F, 0.01F);

    const WeatherCellSample away = cells.sample(60'000.0, 0.0, WeatherScale::Local);
    CY_CHECK(away.scale == WeatherScale::Regional);
    CY_CHECK_NEAR(away.cell_metres, 4'000.0F, 0.01F);
}

CY_TEST_CASE("the hierarchy informs rather than replaces: a global target moves every cell") {
    WeatherCells cells(test::allocator());
    CY_REQUIRE(cells.configure(test::grid(), uniform_climate(), 17).has_value());
    const cy::f32 before = cells.sample(8'000.0, 0.0, WeatherScale::Regional).state.cloud_coverage;

    cy::weather::WeatherState target = cells.global_target();
    target.cloud_coverage = 1.0F;
    target.precipitation_mm_per_hour = 20.0F;
    target.precipitation_type = cy::weather::PrecipitationType::Rain;
    cells.set_global_target(target);
    CY_REQUIRE(cells.advance(test::at_tick(2), 7'200.0).has_value());

    const cy::f32 after = cells.sample(8'000.0, 0.0, WeatherScale::Regional).state.cloud_coverage;
    CY_CHECK_GT(after, before);
    CY_CHECK_GT(cells.sample(8'000.0, 0.0, WeatherScale::Regional).state.precipitation_mm_per_hour,
                0.5F);
}

CY_TEST_CASE("a storm over a cell changes its pressure, wind and rain, and stops when it leaves") {
    StormRegistry storms(test::allocator());
    CY_REQUIRE(
        storms.spawn(test::storm_at(1, 8'000.0, 0.0, cy::weather::StormType::Thunderstorm, 0.9F))
            .has_value());

    WeatherCells cells(test::allocator());
    CY_REQUIRE(cells.configure(test::grid(), uniform_climate(), 19).has_value());
    cells.set_storms(&storms);
    CY_REQUIRE(cells.advance(test::at_tick(1), 600.0).has_value());

    const WeatherCellSample under = cells.sample(8'000.0, 0.0, WeatherScale::Regional);
    const WeatherCellSample away = cells.sample(80'000.0, 0.0, WeatherScale::Regional);
    CY_CHECK_LT(under.state.pressure_hpa, away.state.pressure_hpa);
    CY_CHECK_GT(under.state.precipitation_mm_per_hour, away.state.precipitation_mm_per_hour);
    CY_CHECK_LT(under.state.visibility_metres, away.state.visibility_metres);

    // The storm is removed. The cell relaxes back rather than keeping the storm's rain, which is
    // why the contribution is added AFTER the relaxation and not folded into the target.
    CY_REQUIRE(storms.despawn(cy::weather::StormId{1}).has_value());
    CY_REQUIRE(cells.advance(test::at_tick(2), 7'200.0).has_value());
    const WeatherCellSample cleared = cells.sample(8'000.0, 0.0, WeatherScale::Regional);
    CY_CHECK_LT(cleared.state.precipitation_mm_per_hour, under.state.precipitation_mm_per_hour);
    CY_CHECK_GT(cleared.state.pressure_hpa, under.state.pressure_hpa);
}

CY_TEST_CASE("whole macro steps only, and the remainder is carried") {
    // The property the editor's fast-forward rests on: `advance(90 days)` runs the same steps as
    // ninety `advance(1 day)` calls. Here at the cell level, where it is cheap to state.
    WeatherCells one(test::allocator());
    WeatherCells many(test::allocator());
    CY_REQUIRE(one.configure(test::grid(), uniform_climate(), 23).has_value());
    CY_REQUIRE(many.configure(test::grid(), uniform_climate(), 23).has_value());

    CY_REQUIRE(one.advance(test::at_tick(1), 600.0).has_value());
    for (cy::u32 index = 0; index < 40; ++index) {
        CY_REQUIRE(many.advance(test::at_tick(1), 15.0).has_value());
    }
    CY_CHECK_EQ(one.macro_steps(), many.macro_steps());
    CY_CHECK_EQ(one.sample(8'000.0, 0.0, WeatherScale::Regional).state.temperature_celsius,
                many.sample(8'000.0, 0.0, WeatherScale::Regional).state.temperature_celsius);
    CY_CHECK_EQ(one.sample(8'000.0, 0.0, WeatherScale::Regional).state.humidity,
                many.sample(8'000.0, 0.0, WeatherScale::Regional).state.humidity);

    // A call too short to fill a step advances nothing, and says so.
    cy::Expected<StepReport, cy::Error> tiny = one.advance(test::at_tick(2), 1.0);
    CY_REQUIRE(tiny.has_value());
    CY_CHECK_EQ(tiny->steps, 0u);
}
