// Presets and transitions: weather arrives on per-property clocks, deterministically, through ONE
// entry point that a cinematic and a gameplay trigger share. M10 task 3.2.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`:
// `TransitionProfile::standard()` was changed to give every property the same duration. "a preset
// arrives on per-property clocks, not all at once" went red on the ordering assertions, reporting
// cloud coverage and precipitation at identical progress at every tick. The profile was restored.

#include <cy/test/test.h>

#include <cy/weather/preset.h>

#include <cmath>

#include "fixtures.h"

namespace test = cy::weather::test;
using cy::weather::PrecipitationType;
using cy::weather::Transition;
using cy::weather::TransitionProfile;
using cy::weather::WeatherDirector;
using cy::weather::WeatherPreset;
using cy::weather::WeatherProperty;
using cy::weather::WeatherTarget;

CY_TEST_CASE("a preset describes an environmental state, not a visual configuration") {
    // The specification's own distinction. Every preset is expressed in the units `WeatherState`
    // is: degrees, hectopascals, metres per second, millimetres per hour, metres of visibility.
    const WeatherPreset storm = cy::weather::preset_storm();
    const WeatherPreset clear = cy::weather::preset_clear();
    CY_CHECK_LT(storm.target.pressure_hpa, clear.target.pressure_hpa);
    CY_CHECK_GT(storm.target.wind_speed_mps, clear.target.wind_speed_mps);
    CY_CHECK_GT(storm.target.precipitation_mm_per_hour, clear.target.precipitation_mm_per_hour);
    CY_CHECK_LT(storm.target.visibility_metres, clear.target.visibility_metres);

    const WeatherPreset snow = cy::weather::preset_snowstorm();
    CY_CHECK(snow.target.precipitation_type == PrecipitationType::Snow);
    CY_CHECK_LT(snow.target.temperature_celsius, 0.0F);
    const WeatherPreset sand = cy::weather::preset_sandstorm();
    CY_CHECK(sand.target.precipitation_type == PrecipitationType::Dust);
    CY_CHECK_LT(sand.target.humidity, 0.2F);

    // A target round-trips through the state it drives, which is what makes it the same vocabulary
    // rather than a parallel one.
    const cy::weather::WeatherState state = storm.target.to_state();
    const WeatherTarget back = WeatherTarget::from_state(state);
    CY_CHECK_NEAR(back.wind_speed_mps, storm.target.wind_speed_mps, 0.001F);
    CY_CHECK_NEAR(back.precipitation_mm_per_hour, storm.target.precipitation_mm_per_hour, 0.001F);
}

CY_TEST_CASE("a preset arrives on per-property clocks, not all at once") {
    // "Applying a preset SHALL transition toward it rather than switching, with PER-PROPERTY
    // DURATIONS: cloud coverage over minutes, precipitation over a shorter period, wind over
    // another."
    WeatherDirector director(test::allocator());
    director.set_tick_rate(60.0);
    director.set_immediate(cy::weather::preset_clear().target, 0);
    CY_REQUIRE(
        director.apply(cy::weather::preset_storm(), TransitionProfile::standard(), 0).has_value());

    const Transition& transition = director.transition();
    // At two minutes in, precipitation (120 s) is done and cloud coverage (480 s) is not.
    const cy::u64 two_minutes = 120ULL * 60;
    CY_CHECK_NEAR(transition.progress(WeatherProperty::Precipitation, two_minutes), 1.0F, 0.001F);
    CY_CHECK_LT(transition.progress(WeatherProperty::CloudCoverage, two_minutes), 0.5F);
    CY_CHECK_LT(transition.progress(WeatherProperty::Temperature, two_minutes), 0.2F);
    CY_CHECK_FALSE(transition.complete(two_minutes));

    // The rain arrives before the cloud has finished thickening, which is the behaviour the three
    // durations exist to produce.
    const WeatherTarget midway = director.current(two_minutes);
    CY_CHECK_NEAR(midway.precipitation_mm_per_hour,
                  cy::weather::preset_storm().target.precipitation_mm_per_hour, 0.01F);
    CY_CHECK_LT(midway.cloud_coverage, cy::weather::preset_storm().target.cloud_coverage);

    // And it does arrive: at twenty minutes everything is there.
    CY_CHECK(transition.complete(1'200ULL * 60));
    CY_CHECK_FALSE(director.transitioning(1'200ULL * 60));
}

CY_TEST_CASE("a transition is a pure function of the tick") {
    // The determinism claim: a transition stores its start VALUES and its start TICK and integrates
    // nothing, so a peer that joined late, a rollback that rewound and a replay that fast-forwarded
    // all evaluate the same expression.
    WeatherDirector walked(test::allocator());
    WeatherDirector jumped(test::allocator());
    walked.set_tick_rate(60.0);
    jumped.set_tick_rate(60.0);
    walked.set_immediate(cy::weather::preset_clear().target, 0);
    jumped.set_immediate(cy::weather::preset_clear().target, 0);
    CY_REQUIRE(
        walked.apply(cy::weather::preset_storm(), TransitionProfile::standard(), 0).has_value());
    CY_REQUIRE(
        jumped.apply(cy::weather::preset_storm(), TransitionProfile::standard(), 0).has_value());

    for (cy::u64 tick = 0; tick <= 30'000; tick += 397) {
        (void)walked.current(tick);
    }
    const WeatherTarget stepped = walked.current(18'000);
    const WeatherTarget direct = jumped.current(18'000);
    CY_CHECK_EQ(stepped.cloud_coverage, direct.cloud_coverage);
    CY_CHECK_EQ(stepped.wind_speed_mps, direct.wind_speed_mps);
    CY_CHECK_EQ(stepped.temperature_celsius, direct.temperature_celsius);

    // Re-evaluating an EARLIER tick gives the earlier answer, which an integrator could not do.
    const WeatherTarget rewound = walked.current(600);
    CY_CHECK_LT(rewound.cloud_coverage, stepped.cloud_coverage);
}

CY_TEST_CASE("a timeline and a gameplay trigger are the same operation") {
    // "`sequencing-and-cinematics` drives weather by SETTING THIS STATE ... and a separate
    // cinematic-only weather implementation SHALL NOT exist. A weather change authored on a
    // timeline and one triggered by gameplay SHALL be the same operation."
    //
    // There is one entry point, so "the same operation" is checked by driving it from two callers
    // and comparing the whole CURVE rather than the end point — a parallel path that converged
    // would pass an end-point test.
    WeatherDirector timeline(test::allocator());
    WeatherDirector gameplay(test::allocator());
    timeline.set_tick_rate(60.0);
    gameplay.set_tick_rate(60.0);
    timeline.set_immediate(cy::weather::preset_overcast().target, 100);
    gameplay.set_immediate(cy::weather::preset_overcast().target, 100);

    // The "timeline" schedules it in advance; the "gameplay" applies it when the moment comes.
    CY_REQUIRE(timeline.schedule(cy::weather::preset_storm(), TransitionProfile::standard(), 300)
                   .has_value());
    for (cy::u64 tick = 100; tick <= 6'000; ++tick) {
        CY_REQUIRE(timeline.advance(tick).has_value());
        if (tick == 300) {
            CY_REQUIRE(
                gameplay.apply(cy::weather::preset_storm(), TransitionProfile::standard(), 300)
                    .has_value());
        }
        if (tick % 137 == 0) {
            const WeatherTarget a = timeline.current(tick);
            const WeatherTarget b = gameplay.current(tick);
            CY_CHECK_EQ(a.cloud_coverage, b.cloud_coverage);
            CY_CHECK_EQ(a.precipitation_mm_per_hour, b.precipitation_mm_per_hour);
            CY_CHECK_EQ(a.wind_speed_mps, b.wind_speed_mps);
            CY_CHECK_EQ(a.visibility_metres, b.visibility_metres);
        }
    }
    CY_CHECK_EQ(timeline.scheduled(), 0u);
}

CY_TEST_CASE("a schedule applies once and is reproducible whatever order it was queued in") {
    WeatherDirector forwards(test::allocator());
    WeatherDirector backwards(test::allocator());
    forwards.set_tick_rate(60.0);
    backwards.set_tick_rate(60.0);
    forwards.set_immediate(cy::weather::preset_clear().target, 0);
    backwards.set_immediate(cy::weather::preset_clear().target, 0);

    const TransitionProfile quick = TransitionProfile::uniform(60.0F);
    CY_REQUIRE(forwards.schedule(cy::weather::preset_overcast(), quick, 100).has_value());
    CY_REQUIRE(forwards.schedule(cy::weather::preset_storm(), quick, 2'000).has_value());
    CY_REQUIRE(backwards.schedule(cy::weather::preset_storm(), quick, 2'000).has_value());
    CY_REQUIRE(backwards.schedule(cy::weather::preset_overcast(), quick, 100).has_value());

    for (cy::u64 tick = 0; tick <= 4'000; ++tick) {
        CY_REQUIRE(forwards.advance(tick).has_value());
        CY_REQUIRE(backwards.advance(tick).has_value());
    }
    CY_CHECK_EQ(forwards.current(4'000).precipitation_mm_per_hour,
                backwards.current(4'000).precipitation_mm_per_hour);

    // Idempotent in the tick: re-running a tick does not re-apply a change, which is what a
    // rollback needs and what a frame that ticked twice would otherwise break.
    WeatherDirector rolled(test::allocator());
    rolled.set_tick_rate(60.0);
    rolled.set_immediate(cy::weather::preset_clear().target, 0);
    CY_REQUIRE(rolled.schedule(cy::weather::preset_storm(), quick, 500).has_value());
    for (cy::u64 tick = 0; tick <= 600; ++tick) {
        CY_REQUIRE(rolled.advance(tick).has_value());
    }
    const WeatherTarget once = rolled.current(600);
    for (cy::u32 repeat = 0; repeat < 5; ++repeat) {
        CY_REQUIRE(rolled.advance(600).has_value());
    }
    CY_CHECK_EQ(rolled.current(600).cloud_coverage, once.cloud_coverage);
}

CY_TEST_CASE("interrupting a transition starts from where the weather actually is") {
    WeatherDirector director(test::allocator());
    director.set_tick_rate(60.0);
    director.set_immediate(cy::weather::preset_clear().target, 0);
    CY_REQUIRE(director.apply(cy::weather::preset_storm(), TransitionProfile::uniform(600.0F), 0)
                   .has_value());
    const cy::u64 halfway = 300ULL * 60;
    const WeatherTarget mid = director.current(halfway);
    CY_CHECK_GT(mid.cloud_coverage, cy::weather::preset_clear().target.cloud_coverage);
    CY_CHECK_LT(mid.cloud_coverage, cy::weather::preset_storm().target.cloud_coverage);

    // Turn back toward clear. The new transition starts from the half-storm that is actually
    // outside, not from the storm it was heading for — so nothing snaps.
    CY_REQUIRE(
        director.apply(cy::weather::preset_clear(), TransitionProfile::uniform(600.0F), halfway)
            .has_value());
    CY_CHECK_NEAR(director.current(halfway).cloud_coverage, mid.cloud_coverage, 0.001F);
    CY_CHECK_LT(director.current(halfway + (60ULL * 60)).cloud_coverage, mid.cloud_coverage);
}

CY_TEST_CASE("an individual parameter can be set without cancelling the transition") {
    // "target preset, transition timing, and INDIVIDUAL PARAMETERS" — the third of those.
    WeatherDirector director(test::allocator());
    director.set_tick_rate(60.0);
    director.set_immediate(cy::weather::preset_clear().target, 0);
    CY_REQUIRE(director.apply(cy::weather::preset_storm(), TransitionProfile::uniform(600.0F), 0)
                   .has_value());
    const cy::u64 now = 60ULL * 60;
    CY_REQUIRE(director.apply_property(WeatherProperty::WindSpeed, 40.0F, 30.0F, now).has_value());

    // The wind gets there quickly...
    CY_CHECK_NEAR(director.current(now + (30ULL * 60)).wind_speed_mps, 40.0F, 0.1F);
    // ...and the storm is still arriving on its own clock rather than having been cancelled.
    CY_CHECK_LT(director.current(now + (30ULL * 60)).cloud_coverage,
                cy::weather::preset_storm().target.cloud_coverage);
    CY_CHECK_GT(director.current(now + (30ULL * 60)).cloud_coverage,
                director.current(now).cloud_coverage);
}

CY_TEST_CASE("a veering wind takes the short way round") {
    // A bearing interpolated the long way would swing a whole world's trees backwards for half a
    // minute; a wind VECTOR interpolated linearly would pass through calm. Neither happens, which
    // is why `WeatherTarget` carries speed and bearing separately.
    WeatherDirector director(test::allocator());
    director.set_tick_rate(60.0);
    WeatherTarget from = cy::weather::preset_clear().target;
    from.wind_bearing_radians = 3.0F;
    from.wind_speed_mps = 10.0F;
    director.set_immediate(from, 0);

    WeatherTarget to = from;
    to.wind_bearing_radians = -3.0F;  // 0.28 rad away across the wrap, not 6.0 the other way
    CY_REQUIRE(director.apply(to, TransitionProfile::uniform(60.0F), 0).has_value());

    for (cy::u64 tick = 0; tick <= 60ULL * 60; tick += 60) {
        const cy::f32 bearing = director.current(tick).wind_bearing_radians;
        // Never passes through zero: the short way from 3.0 to -3.0 goes up through pi and wraps.
        CY_CHECK(std::fabs(bearing) > 2.5F);
        // And the speed never dips, which a vector interpolation would have made it do.
        CY_CHECK_NEAR(director.current(tick).wind_speed_mps, 10.0F, 0.001F);
    }
}
