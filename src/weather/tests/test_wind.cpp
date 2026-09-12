// The wind field: one composition, two determinism classes, a budget whose drop is deterministic.
// M10 task 3.1.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`:
// `enforce_budget()`'s comparator was changed from (priority descending, identity ascending) to a
// plain `remove_unordered` of the tail, which is registration order. "the transient budget drops
// the same sources whichever order they arrived in" went red immediately, with the forwards run
// keeping {1,2,3} and the backwards run keeping {6,5,4}. The comparator was restored.

#include <cy/test/test.h>

#include <cy/weather/climate.h>
#include <cy/weather/diagnostics.h>
#include <cy/weather/storm.h>
#include <cy/weather/wind.h>

#include <cmath>

#include "fixtures.h"

namespace test = cy::weather::test;
using cy::weather::ClimateMap;
using cy::weather::StormRegistry;
using cy::weather::TransientWindSource;
using cy::weather::WeatherCells;
using cy::weather::WindComposer;
using cy::weather::WindContribution;
using cy::weather::WindSample;
using cy::weather::WindSourceId;
using cy::weather::WindSourceKind;
using cy::weather::WindVolume;
using cy::weather::WindVolumeKind;

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

CY_TEST_CASE("every reader agrees about the authoritative wind and only one may see turbulence") {
    // THE CENTRAL REQUIREMENT, and the reason wind.h splits the sample into three components:
    // "WHEN trees, smoke, water, and cloth are visible together THEN all SHALL sample one field and
    // move consistently", together with "Visual weather detail SHALL NOT influence authoritative
    // state."
    WeatherCells cells(test::allocator());
    CY_REQUIRE(cells.configure(test::grid(), uniform_climate(), 29).has_value());
    CY_REQUIRE(cells.advance(test::at_tick(1), 600.0).has_value());

    WindComposer wind(test::allocator());
    wind.set_cells(&cells);
    wind.set_seed(0xFEEDULL);

    for (cy::u32 index = 0; index < 64; ++index) {
        const cy::world::WorldVec3d at{static_cast<cy::f64>(index) * 311.0, 4.0,
                                       static_cast<cy::f64>(index) * -173.0};
        const WindSample gameplay = wind.sample(at, cy::determinism::SimulationClass::Authoritative,
                                                test::at_tick(9), 12.5);
        const WindSample presented =
            wind.sample(at, cy::determinism::SimulationClass::Presentation, test::at_tick(9), 12.5);

        // BIT FOR BIT. Not "close": the projectile and the tree must agree exactly, or a replay
        // that renders differently on two clients diverges.
        CY_CHECK_EQ(gameplay.authoritative().x, presented.authoritative().x);
        CY_CHECK_EQ(gameplay.authoritative().y, presented.authoritative().y);
        CY_CHECK_EQ(gameplay.authoritative().z, presented.authoritative().z);

        // And the authoritative reader carries no turbulence at all — the firewall, at the one
        // place the two classes meet.
        CY_CHECK_EQ(gameplay.turbulence.x, 0.0F);
        CY_CHECK_EQ(gameplay.turbulence.y, 0.0F);
        CY_CHECK_EQ(gameplay.turbulence.z, 0.0F);
    }

    // The turbulence is not merely absent from one reader: it is PRESENT for the other, or the
    // equality above would be trivially true of a model that computes nothing.
    cy::f32 seen = 0.0F;
    for (cy::u32 index = 0; index < 64; ++index) {
        const cy::world::WorldVec3d at{static_cast<cy::f64>(index) * 7.0, 3.0, 11.0};
        seen += cy::length(
            wind.sample(at, cy::determinism::SimulationClass::Presentation, test::at_tick(9), 12.5)
                .turbulence);
    }
    CY_CHECK_GT(seen, 0.0F);
}

CY_TEST_CASE("the gust is authoritative, continuous in time, and a pure function of the tick") {
    WeatherCells cells(test::allocator());
    CY_REQUIRE(cells.configure(test::grid(), uniform_climate(), 31).has_value());
    CY_REQUIRE(cells.advance(test::at_tick(1), 600.0).has_value());

    WindComposer wind(test::allocator());
    wind.set_cells(&cells);
    wind.set_seed(0x1234ULL);
    const cy::world::WorldVec3d at{100.0, 2.0, 50.0};

    // A gust drawn twice at one moment is one gust — there is no cursor inside the stream, which is
    // `random.h`'s whole design and the reason a replay can re-evaluate rather than record.
    const WindSample once =
        wind.sample(at, cy::determinism::SimulationClass::Authoritative, test::at_tick(40), 4.0);
    const WindSample twice =
        wind.sample(at, cy::determinism::SimulationClass::Authoritative, test::at_tick(40), 4.0);
    CY_CHECK_EQ(once.gust.x, twice.gust.x);
    CY_CHECK_EQ(once.gust.z, twice.gust.z);

    // Continuity: across a gust-cycle boundary the value moves by a fraction of its own amplitude,
    // not by its whole range. A gust that snapped between cycles would snap every tree at once.
    const cy::f32 period = 7.0F;
    const cy::f64 just_before = static_cast<cy::f64>(period) - 0.01;
    const cy::f64 just_after = static_cast<cy::f64>(period) + 0.01;
    const WindSample before = wind.sample(at, cy::determinism::SimulationClass::Authoritative,
                                          test::at_tick(41), just_before);
    const WindSample after = wind.sample(at, cy::determinism::SimulationClass::Authoritative,
                                         test::at_tick(41), just_after);
    const cy::Vec3 jump{after.gust.x - before.gust.x, after.gust.y - before.gust.y,
                        after.gust.z - before.gust.z};
    CY_CHECK_LT(cy::length(jump), 0.5F);

    // A different seed is a different world's gust. If it were not, the gust would not be derived
    // from the session seed at all.
    WindComposer other(test::allocator());
    other.set_cells(&cells);
    other.set_seed(0x5678ULL);
    const WindSample elsewhere =
        other.sample(at, cy::determinism::SimulationClass::Authoritative, test::at_tick(40), 4.0);
    CY_CHECK_NE(once.gust.x, elsewhere.gust.x);
}

CY_TEST_CASE("the six volume shapes each do what their name says") {
    WindVolume directional;
    directional.kind = WindVolumeKind::Directional;
    directional.position = cy::world::WorldVec3d{0.0, 0.0, 0.0};
    directional.radius_metres = 20.0F;
    directional.strength = 12.0F;
    directional.direction = cy::Vec3{0.0F, 0.0F, 1.0F};
    directional.falloff = cy::weather::WindFalloff::Constant;
    cy::f32 influence = 0.0F;
    const cy::Vec3 pushed = cy::weather::wind_volume_velocity(
        directional, cy::world::WorldVec3d{1.0, 0.0, 0.0}, &influence);
    CY_CHECK_GT(influence, 0.0F);
    CY_CHECK_GT(pushed.z, 10.0F);
    // Outside the radius it contributes nothing at all — which is what keeps the composition's cost
    // proportional to the volumes near the sample.
    cy::f32 outside = 0.0F;
    const cy::Vec3 nothing = cy::weather::wind_volume_velocity(
        directional, cy::world::WorldVec3d{500.0, 0.0, 0.0}, &outside);
    CY_CHECK_EQ(outside, 0.0F);
    CY_CHECK_EQ(cy::length(nothing), 0.0F);

    WindVolume updraft = directional;
    updraft.kind = WindVolumeKind::Updraft;
    CY_CHECK_GT(
        cy::weather::wind_volume_velocity(updraft, cy::world::WorldVec3d{1.0, 0.0, 0.0}, &influence)
            .y,
        0.0F);
    WindVolume downdraft = updraft;
    downdraft.kind = WindVolumeKind::Downdraft;
    CY_CHECK_LT(cy::weather::wind_volume_velocity(downdraft, cy::world::WorldVec3d{1.0, 0.0, 0.0},
                                                  &influence)
                    .y,
                0.0F);

    // A VORTEX rotates: the velocity at a point is perpendicular to the radius, up to the declared
    // inward and lift shares. A tornado that blew outward would not be a tornado.
    WindVolume vortex = directional;
    vortex.kind = WindVolumeKind::Vortex;
    vortex.direction = cy::Vec3{0.0F, 1.0F, 0.0F};
    vortex.inward = 0.0F;
    vortex.lift = 0.0F;
    const cy::Vec3 around =
        cy::weather::wind_volume_velocity(vortex, cy::world::WorldVec3d{5.0, 0.0, 0.0}, &influence);
    CY_CHECK_NEAR(around.x, 0.0F, 0.1F);
    CY_CHECK_GT(std::fabs(around.z), 1.0F);

    // A BLAST is outward, and a SPLINE follows the polyline's own direction rather than pointing at
    // its end — which is what makes a canyon wind run along the canyon.
    WindVolume blast = directional;
    blast.kind = WindVolumeKind::RadialBlast;
    const cy::Vec3 out =
        cy::weather::wind_volume_velocity(blast, cy::world::WorldVec3d{6.0, 0.0, 0.0}, &influence);
    CY_CHECK_GT(out.x, 0.0F);

    WindVolume spline;
    spline.kind = WindVolumeKind::Spline;
    spline.radius_metres = 30.0F;
    spline.strength = 9.0F;
    spline.falloff = cy::weather::WindFalloff::Constant;
    spline.points[0] = cy::world::WorldVec3d{0.0, 0.0, 0.0};
    spline.points[1] = cy::world::WorldVec3d{100.0, 0.0, 0.0};
    spline.points[2] = cy::world::WorldVec3d{100.0, 0.0, 100.0};
    spline.point_count = 3;
    const cy::Vec3 along_first = cy::weather::wind_volume_velocity(
        spline, cy::world::WorldVec3d{40.0, 0.0, 5.0}, &influence);
    const cy::Vec3 along_second = cy::weather::wind_volume_velocity(
        spline, cy::world::WorldVec3d{105.0, 0.0, 60.0}, &influence);
    CY_CHECK_GT(along_first.x, 5.0F);
    CY_CHECK_GT(along_second.z, 5.0F);
}

CY_TEST_CASE("the transient budget drops the same sources whichever order they arrived in") {
    // "bounded by a budget, with the lowest-priority sources dropped DETERMINISTICALLY when it is
    // exceeded" — the debt `environment-fields` left this row, and the whole reason the drop is a
    // total order over (priority, identity) rather than over array position.
    const auto build = [](cy::u64 id, cy::u16 priority) {
        TransientWindSource source;
        source.id = WindSourceId{id};
        source.volume.kind = WindVolumeKind::RadialBlast;
        source.volume.position = cy::world::WorldVec3d{static_cast<cy::f64>(id) * 10.0, 0.0, 0.0};
        source.volume.radius_metres = 15.0F;
        source.volume.strength = 20.0F;
        source.volume.priority = priority;
        source.lifetime_seconds = 100.0F;
        return source;
    };

    WindComposer forwards(test::allocator());
    WindComposer backwards(test::allocator());
    forwards.set_transient_budget(3);
    backwards.set_transient_budget(3);
    for (cy::u64 id = 1; id <= 6; ++id) {
        CY_REQUIRE(forwards.add_transient(build(id, static_cast<cy::u16>(id))).has_value());
    }
    for (cy::u64 id = 6; id >= 1; --id) {
        CY_REQUIRE(backwards.add_transient(build(id, static_cast<cy::u16>(id))).has_value());
    }

    CY_REQUIRE_EQ(forwards.transients().size(), 3u);
    CY_REQUIRE_EQ(backwards.transients().size(), 3u);
    for (cy::usize index = 0; index < 3; ++index) {
        CY_CHECK_EQ(forwards.transients()[index].id.value, backwards.transients()[index].id.value);
    }
    // The three highest priorities survive, and the same number were dropped either way.
    CY_CHECK_EQ(forwards.transients()[0].id.value, 6u);
    CY_CHECK_GT(forwards.dropped_count(), 0u);
    CY_CHECK_EQ(forwards.dropped_count(), backwards.dropped_count());
    // `last_dropped()` is deliberately NOT compared: it is the last of a sequence of drops, and the
    // sequence depends on the order registrations arrived in. What must be deterministic is the SET
    // that survives — see wind.h — and that is what the loop above requires.
}

CY_TEST_CASE("a transient source expires and stops contributing") {
    WindComposer wind(test::allocator());
    TransientWindSource blast;
    blast.id = WindSourceId{99};
    blast.volume.kind = WindVolumeKind::Directional;
    blast.volume.position = cy::world::WorldVec3d{0.0, 0.0, 0.0};
    blast.volume.radius_metres = 40.0F;
    blast.volume.strength = 30.0F;
    blast.volume.direction = cy::Vec3{1.0F, 0.0F, 0.0F};
    blast.volume.falloff = cy::weather::WindFalloff::Constant;
    blast.lifetime_seconds = 2.0F;
    blast.fade = false;
    CY_REQUIRE(wind.add_transient(blast).has_value());

    const cy::world::WorldVec3d at{5.0, 0.0, 0.0};
    const cy::f32 during =
        wind.sample(at, cy::determinism::SimulationClass::Authoritative, test::at_tick(1), 0.0)
            .base.x;
    CY_CHECK_GT(during, 20.0F);

    CY_REQUIRE(wind.advance(3.0F).has_value());
    CY_CHECK_EQ(wind.transients().size(), 0u);
    const cy::f32 after =
        wind.sample(at, cy::determinism::SimulationClass::Authoritative, test::at_tick(2), 3.0)
            .base.x;
    CY_CHECK_LT(after, 1.0F);
}

CY_TEST_CASE("terrain blocks, channels and accelerates the wind") {
    WeatherCells cells(test::allocator());
    CY_REQUIRE(cells.configure(test::grid(), uniform_climate(), 37).has_value());
    CY_REQUIRE(cells.advance(test::at_tick(1), 600.0).has_value());

    WindComposer flat(test::allocator());
    flat.set_cells(&cells);
    flat.set_seed(1);
    WindComposer hilly(test::allocator());
    hilly.set_cells(&cells);
    hilly.set_seed(1);
    hilly.set_terrain(test::ridge());

    // On the windward slope the wind is forced up and over: the vertical component appears and the
    // horizontal speed rises. On the lee it is blocked, and both fall.
    const cy::world::WorldVec3d up_slope{16'000.0, 10.0, 0.0};
    const cy::world::WorldVec3d down_slope{24'000.0, 10.0, 0.0};
    const WindSample flat_up = flat.sample(
        up_slope, cy::determinism::SimulationClass::Authoritative, test::at_tick(1), 0.0);
    const WindSample ridge_up = hilly.sample(
        up_slope, cy::determinism::SimulationClass::Authoritative, test::at_tick(1), 0.0);
    const WindSample ridge_down = hilly.sample(
        down_slope, cy::determinism::SimulationClass::Authoritative, test::at_tick(1), 0.0);

    CY_CHECK_NEAR(flat_up.base.y, 0.0F, 0.001F);
    CY_CHECK_GT(ridge_up.base.y, 0.1F);
    CY_CHECK_LT(ridge_down.base.y, -0.1F);
    CY_CHECK_GT(ridge_up.base.x, ridge_down.base.x);
}

CY_TEST_CASE("the explanation sums to the sample, exactly") {
    // diagnostics.h's claim, checked at the source: `explain()` and `sample()` are two consumers of
    // ONE walk, so the reported contributions add up to the reported wind and cannot describe a
    // wind the simulation did not produce.
    WeatherCells cells(test::allocator());
    CY_REQUIRE(cells.configure(test::grid(), uniform_climate(), 41).has_value());
    cells.set_terrain(test::ridge());
    CY_REQUIRE(cells.advance(test::at_tick(1), 600.0).has_value());

    StormRegistry storms(test::allocator());
    CY_REQUIRE(storms.spawn(test::storm_at(4, 16'000.0, 0.0, cy::weather::StormType::Cyclone, 0.8F))
                   .has_value());

    WindComposer wind(test::allocator());
    wind.set_cells(&cells);
    wind.set_storms(&storms);
    wind.set_terrain(test::ridge());
    wind.set_seed(0xAAAAULL);

    WindVolume tornado;
    tornado.kind = WindVolumeKind::Vortex;
    tornado.position = cy::world::WorldVec3d{16'000.0, 0.0, 0.0};
    tornado.radius_metres = 200.0F;
    tornado.strength = 60.0F;
    tornado.direction = cy::Vec3{0.0F, 1.0F, 0.0F};
    CY_REQUIRE(wind.add_volume(tornado).has_value());

    const cy::world::WorldVec3d at{16'050.0, 12.0, 30.0};
    WindContribution sources[cy::weather::kMaxInspectedSources];
    const cy::usize count =
        wind.explain(at, cy::determinism::SimulationClass::Presentation, test::at_tick(5), 2.5,
                     cy::Span<WindContribution>(sources, cy::weather::kMaxInspectedSources));
    CY_REQUIRE((count) > (4u));

    cy::Vec3 total;
    bool saw_storm = false;
    bool saw_volume = false;
    bool saw_terrain = false;
    for (cy::usize index = 0; index < count; ++index) {
        total.x += sources[index].velocity.x;
        total.y += sources[index].velocity.y;
        total.z += sources[index].velocity.z;
        saw_storm = saw_storm || sources[index].kind == WindSourceKind::Storm;
        saw_volume = saw_volume || sources[index].kind == WindSourceKind::Volume;
        saw_terrain = saw_terrain || sources[index].kind == WindSourceKind::Terrain;
    }
    CY_CHECK(saw_storm);
    CY_CHECK(saw_volume);
    CY_CHECK(saw_terrain);

    const WindSample sample =
        wind.sample(at, cy::determinism::SimulationClass::Presentation, test::at_tick(5), 2.5);
    CY_CHECK_EQ(total.x, sample.full().x);
    CY_CHECK_EQ(total.y, sample.full().y);
    CY_CHECK_EQ(total.z, sample.full().z);
}

CY_TEST_CASE("a batch of four thousand winds matches the same samples taken one at a time") {
    WeatherCells cells(test::allocator());
    CY_REQUIRE(cells.configure(test::grid(), uniform_climate(), 43).has_value());
    CY_REQUIRE(cells.advance(test::at_tick(1), 600.0).has_value());
    WindComposer wind(test::allocator());
    wind.set_cells(&cells);
    wind.set_seed(9);

    cy::Array<cy::world::WorldVec3d> positions(test::allocator());
    cy::Array<WindSample> out(test::allocator());
    CY_REQUIRE(positions.resize(4'000).has_value());
    CY_REQUIRE(out.resize(4'000).has_value());
    for (cy::usize index = 0; index < positions.size(); ++index) {
        positions[index] = cy::world::WorldVec3d{static_cast<cy::f64>(index) * 3.0, 1.0,
                                                 static_cast<cy::f64>(index % 97) * 11.0};
    }
    CY_REQUIRE(wind.sample_many(positions.span(), cy::determinism::SimulationClass::Authoritative,
                                test::at_tick(2), 1.0, out.span())
                   .has_value());
    for (cy::usize index = 0; index < positions.size(); index += 331) {
        const WindSample one =
            wind.sample(positions[index], cy::determinism::SimulationClass::Authoritative,
                        test::at_tick(2), 1.0);
        CY_CHECK_EQ(one.base.x, out[index].base.x);
        CY_CHECK_EQ(one.gust.z, out[index].gust.z);
    }
    // A short output is refused rather than truncated.
    CY_CHECK_FALSE(wind.sample_many(positions.span(),
                                    cy::determinism::SimulationClass::Authoritative,
                                    test::at_tick(2), 1.0, cy::Span<WindSample>(out.begin(), 10))
                       .has_value());
}
