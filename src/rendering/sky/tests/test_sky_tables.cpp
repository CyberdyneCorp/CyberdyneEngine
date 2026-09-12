// THE FOUR TABLES, AND THE TWO HALVES M7's SEED NAMED AS GAPS.
//
// Integration and not unit, because every claim in this file is an INTEGRAL: a transmittance table
// against the ray march it replaces, a multiple-scattering table against the isotropic constant it
// replaces, and an incremental sky view against a full rebuild of the same table over a simulated
// day. Each of those is thousands of atmosphere evaluations, and a version cheap enough for a 1 ms
// budget would be a spot check — which is exactly what would have passed the version of this module
// that had no multiple-scattering table at all.

#include <cy/test/test.h>

#include <cy/core/math/math.h>
#include <cy/rendering/sky/composition.h>
#include <cy/rendering/sky/diagnostics.h>
#include <cy/rendering/sky/tables.h>

#include <cmath>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec2;
using cy::Vec3;
using cy::rendering::FroxelVolume;
using namespace cy::rendering::sky;

[[nodiscard]] f32 relative_difference(Vec3 a, Vec3 b) {
    f32 worst = 0.0F;
    for (u32 channel = 0; channel < 3; ++channel) {
        const f32 reference = cy::math::max(std::fabs(b[channel]), 1.0e-6F);
        worst = cy::math::max(worst, std::fabs(a[channel] - b[channel]) / reference);
    }
    return worst;
}

[[nodiscard]] Vec3 direction_at(f32 elevation_degrees, f32 azimuth_degrees) {
    const f32 elevation = elevation_degrees * cy::math::kDegToRad;
    const f32 azimuth = azimuth_degrees * cy::math::kDegToRad;
    return Vec3{std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
                std::cos(elevation) * std::sin(azimuth)};
}

}  // namespace

CY_TEST_CASE("sky tables: transmittance is tabulated, reused, and agrees with the march") {
    const Atmosphere atmosphere = earth_atmosphere();
    TransmittanceTable table;
    CY_REQUIRE(table.configure(SkyTableQuality::High));

    auto built = table.build(atmosphere);
    CY_REQUIRE(built);
    CY_CHECK(built.value());

    // "Tables SHALL be regenerated only when the parameters they depend on change." A second build
    // of the same atmosphere is a reuse, and the counter is what makes that checkable rather than
    // assumed.
    auto again = table.build(atmosphere);
    CY_REQUIRE(again);
    CY_CHECK_FALSE(again.value());
    CY_CHECK_EQ(table.stats().full_rebuilds, 1U);
    CY_CHECK_EQ(table.stats().reuses, 1U);

    // A changed atmosphere IS a rebuild. Without this the reuse above would be indistinguishable
    // from a table that never rebuilds.
    Atmosphere dusty = thin_dusty_atmosphere();
    auto rebuilt = table.build(dusty);
    CY_REQUIRE(rebuilt);
    CY_CHECK(rebuilt.value());
    CY_CHECK_EQ(table.stats().full_rebuilds, 2U);

    CY_REQUIRE(table.build(atmosphere));

    // And it answers what the march answers. The table exists to replace the march, so the
    // comparison is over a spread of altitudes and angles rather than at one convenient point.
    f32 worst = 0.0F;
    for (const f32 altitude : {0.0F, 500.0F, 4000.0F, 20000.0F}) {
        for (const f32 cosine : {1.0F, 0.7F, 0.3F, 0.05F}) {
            const Vec3 position = ground_position(atmosphere, altitude);
            const f32 sine = std::sqrt(cy::math::max(1.0F - (cosine * cosine), 0.0F));
            const Vec3 direction{sine, cosine, 0.0F};
            const Vec3 marched = transmittance(atmosphere, position, direction, 64);
            worst =
                cy::math::max(worst, relative_difference(table.sample(altitude, cosine), marched));
        }
    }
    CY_TEST_MESSAGE("transmittance table against the march: worst relative difference ",
                    worst * 100.0F, "%");
    CY_CHECK_LT(worst, 0.06F);
}

CY_TEST_CASE("sky tables: multiple scattering is a table, and the table can be switched off") {
    const Atmosphere atmosphere = earth_atmosphere();

    // A table derived from an unbuilt transmittance would be zero everywhere, and zero looks like a
    // plausible answer. It is refused instead.
    TransmittanceTable empty;
    MultipleScatteringTable refused;
    CY_REQUIRE(refused.configure(SkyTableQuality::Low));
    CY_CHECK_FALSE(refused.build(atmosphere, empty));

    AtmosphereTables both;
    CY_REQUIRE(both.configure(SkyTableQuality::High));
    auto built = both.build(atmosphere);
    CY_REQUIRE(built);
    CY_CHECK(built.value());
    CY_CHECK(both.built());
    CY_CHECK_GT(both.directions_integrated(), 0ULL);

    // THE A/B. An `AtmosphereTables` whose multiple-scattering half was never built samples zero
    // there, so the SAME function, over the SAME transmittance, evaluates single scattering alone.
    // That is the only honest way to say what the table contributes: comparing it against
    // `sky_radiance()`'s isotropic constant would confound the table with the constant's own error.
    AtmosphereTables single;
    CY_REQUIRE(single.configure(SkyTableQuality::High));
    CY_REQUIRE(single.transmittance.build(atmosphere));
    CY_CHECK_FALSE(single.built());

    const Vec3 ground = ground_position(atmosphere, 0.0F);
    struct Probe {
        const char* name = "";
        Vec3 view{0.0F, 1.0F, 0.0F};
        Vec3 sun{0.0F, 1.0F, 0.0F};
    };
    const Probe probes[] = {
        {"zenith at noon", direction_at(90.0F, 0.0F), direction_at(60.0F, 0.0F)},
        {"horizon away from a low sun", direction_at(2.0F, 90.0F), direction_at(1.0F, 0.0F)},
        {"a slope facing away from the sun", direction_at(20.0F, 180.0F),
         direction_at(60.0F, 0.0F)},
    };

    f32 smallest_share = 1.0F;
    for (const Probe& probe : probes) {
        const Vec3 with =
            sky_radiance_tabulated(atmosphere, both, ground, probe.view, probe.sun, 48);
        const Vec3 without =
            sky_radiance_tabulated(atmosphere, single, ground, probe.view, probe.sun, 48);
        const f32 share = (with.y - without.y) / cy::math::max(with.y, 1.0e-9F);
        CY_TEST_MESSAGE(with.y, " nits with the table, ", without.y,
                        " without — multiple scattering is ", share * 100.0F, "% of it");
        CY_CHECK_GT(with.y, without.y);
        smallest_share = cy::math::min(smallest_share, share);
    }
    // A fifth of a clear sky is light that has bounced more than once. A "table" contributing one
    // per cent would be a table nobody needed, and that is the failure this bound catches.
    CY_CHECK_GT(smallest_share, 0.15F);

    // AND IT IS EXACTLY ZERO WHERE NO SUNLIGHT REACHES THE COLUMN. Multiply-scattered light is
    // still light that came from the star, and a table indexed by the sun's own elevation can say
    // so. See the midnight case below for what the alternative cost.
    CY_CHECK_EQ(both.multiple_scattering.sample(0.0F, -0.35F).y, 0.0F);
    CY_CHECK_GT(both.multiple_scattering.sample(0.0F, 0.5F).y, 0.0F);
}

CY_TEST_CASE("sky tables: a sun twenty degrees below the horizon lights nothing") {
    // REGRESSION. Until M10 the isotropic multiple-scattering stand-in in `sky_radiance()` was
    // added wherever the view ray passed, WITHOUT the sun's own transmittance — so it was added to
    // air no sunlight reaches. Measured on the tree this test was written against, a sun twenty
    // degrees below the horizon produced 378 nits at the zenith and 2 679 nits at the horizon,
    // which is daylight brightness in a scene that is supposed to be dark, and which no tone mapper
    // can recover from. Both paths are checked, because the defect was in the one M7 shipped and
    // the fix has to hold for the one M10 added.
    const Atmosphere atmosphere = earth_atmosphere();
    AtmosphereTables tables;
    CY_REQUIRE(tables.configure(SkyTableQuality::Medium));
    CY_REQUIRE(tables.build(atmosphere));

    const Vec3 ground = ground_position(atmosphere, 0.0F);
    const Vec3 night = direction_at(-20.0F, 0.0F);
    f32 worst_night = 0.0F;
    for (const f32 elevation : {90.0F, 30.0F, 3.0F}) {
        const Vec3 view = direction_at(elevation, 90.0F);
        const Vec3 marched = sky_radiance(atmosphere, ground, view, night, 48);
        const Vec3 tabulated = sky_radiance_tabulated(atmosphere, tables, ground, view, night, 48);
        // The marched path reaches EXACTLY zero, because its shadow test is a ray-sphere
        // intersection. The tabulated one carries a millionth of a nit, because a bilinear fetch
        // from the transmittance table smears the shadow boundary across one texel — which is a
        // property of every table and is six orders of magnitude below anything a tone mapper can
        // see. Both are checked against a THRESHOLD rather than against zero for that reason, and
        // the threshold is still five orders of magnitude below the defect it replaces.
        CY_CHECK_EQ(marched.y, 0.0F);
        worst_night = cy::math::max(worst_night, tabulated.y);
    }
    CY_TEST_MESSAGE("midnight sky: marched 0 nits, tabulated at most ", worst_night,
                    " nits (the defect this replaces measured 2 679)");
    CY_CHECK_LT(worst_night, 1.0e-3F);
    // And the sun is still the sun in daylight: a fix that darkened everything would pass the three
    // checks above and be worse than the defect.
    const Vec3 day = direction_at(45.0F, 0.0F);
    CY_CHECK_GT(sky_radiance(atmosphere, ground, direction_at(60.0F, 90.0F), day, 48).y, 100.0F);

    // With the sun's transmittance applied, M7's factor of 0.35 turns out to be a good guess in
    // daylight: the constant and the table agree to within a few per cent where the sun can reach.
    // It was never the daylight value that was wrong.
    const Vec3 view = direction_at(90.0F, 0.0F);
    const Vec3 constant = sky_radiance(atmosphere, ground, view, day, 48);
    const Vec3 tabulated = sky_radiance_tabulated(atmosphere, tables, ground, view, day, 48);
    CY_TEST_MESSAGE("daylight zenith: isotropic constant ", constant.y, " nits, table ",
                    tabulated.y);
    CY_CHECK_LT(relative_difference(constant, tabulated), 0.15F);
}

CY_TEST_CASE("sky tables: the sky view regenerates incrementally as the sun moves") {
    const Atmosphere atmosphere = earth_atmosphere();
    const Vec3 ground = ground_position(atmosphere, 0.0F);

    // A quarter of a day, dawn to noon, in ninety-six updates: 0.92 degrees of sun movement each,
    // which is a day compressed to about six seconds at sixty frames a second and roughly two
    // hundred times faster than a project's twenty-minute day. The compression is deliberate — the
    // failure this scheme can have is a row nobody ever chooses, and a realistic rate would hide it
    // behind a sun that barely moves.
    constexpr u32 kSteps = 96;
    constexpr f32 kSweepDegrees = 88.0F;

    // THE ERROR IS MEASURED AGAINST THE BRIGHTEST SKY OF THE MOMENT, not per sample. A sample below
    // the horizon at dusk is a handful of nits, and a relative error on it is enormous and
    // invisible; what a viewer sees is the error as a fraction of what else is on the screen.
    struct Run {
        u32 budget = 0;
        f32 worst_error = 0.0F;
        f32 worst_staleness = 0.0F;
        f32 rebuild_fraction = 0.0F;
        u32 full_rebuilds_after_first = 0;
    };

    Run runs[3];
    const u32 budgets[3] = {1, 3, 8};
    for (u32 index = 0; index < 3; ++index) {
        IncrementalSkyView incremental;
        CY_REQUIRE(incremental.configure(SkyTableQuality::Low));
        SkyViewTable oracle;
        CY_REQUIRE(oracle.configure(SkyTableQuality::Low));
        Run& run = runs[index];
        run.budget = budgets[index];

        for (u32 step = 0; step <= kSteps; ++step) {
            const f32 elevation =
                (static_cast<f32>(step) / static_cast<f32>(kSteps)) * kSweepDegrees;
            const Vec3 sun = direction_at(elevation, 30.0F);

            auto update = incremental.update(atmosphere, ground, sun, run.budget);
            CY_REQUIRE(update);
            CY_CHECK_LE(update.value().rows_rebuilt, step == 0 ? incremental.rows() : run.budget);
            if (step > 0 && update.value().full_rebuild) {
                ++run.full_rebuilds_after_first;
            }
            CY_REQUIRE(oracle.update(atmosphere, ground, sun));

            f32 peak = 1.0e-6F;
            f32 error = 0.0F;
            for (const f32 elevation_sample : {-5.0F, 1.0F, 8.0F, 30.0F, 70.0F}) {
                for (const f32 azimuth : {0.0F, 90.0F, 210.0F}) {
                    const Vec3 direction = direction_at(elevation_sample, azimuth);
                    const Vec3 got = incremental.sample(direction);
                    const Vec3 want = oracle.sample(direction);
                    for (u32 channel = 0; channel < 3; ++channel) {
                        peak = cy::math::max(peak, std::fabs(want[channel]));
                        error = cy::math::max(error, std::fabs(got[channel] - want[channel]));
                    }
                }
            }
            run.worst_error = cy::math::max(run.worst_error, error / peak);
        }

        const TableDiagnostics diagnostics =
            table_diagnostics(AtmosphereTables{}, incremental, 1.0F);
        run.worst_staleness = diagnostics.worst_row_staleness;
        run.rebuild_fraction = diagnostics.mean_rebuild_fraction;
        CY_CHECK_EQ(incremental.stats().full_rebuilds, 1U);
        CY_CHECK_GT(incremental.stats().incremental_updates, 80U);
        CY_TEST_MESSAGE("budget ", run.budget, " of ", incremental.rows(),
                        " rows: ", run.rebuild_fraction * 100.0F,
                        "% of a rebuild per update, worst error ", run.worst_error * 100.0F,
                        "% of the brightest sky, worst staleness ",
                        run.worst_staleness * cy::math::kRadToDeg, " degrees of sun movement");
    }

    // A MOVING SUN NEVER FORCES A FULL REBUILD. That is the requirement's own sentence, and it is
    // the one thing an "incremental" table can get wrong while still looking incremental.
    for (const Run& run : runs) {
        CY_CHECK_EQ(run.full_rebuilds_after_first, 0U);
    }

    // THE TRADE-OFF IS THE MEASUREMENT. A single bound could be met by a scheme that rebuilds
    // everything, or by one whose budget does nothing; what says the budget is a real lever is that
    // spending more of it buys accuracy, monotonically, in both the error and the staleness bound.
    CY_CHECK_LT(runs[1].worst_error, runs[0].worst_error);
    CY_CHECK_LT(runs[2].worst_error, runs[1].worst_error);
    CY_CHECK_LT(runs[1].worst_staleness, runs[0].worst_staleness);
    CY_CHECK_LT(runs[2].worst_staleness, runs[1].worst_staleness);

    // And the shipping range: three rows of sixteen is under a fifth of a rebuild per update and
    // stays within a quarter of the brightest sky at two hundred times a real day's sun rate; half
    // a rebuild holds it inside a tenth.
    CY_CHECK_LT(runs[1].rebuild_fraction, 0.2F);
    CY_CHECK_LT(runs[1].worst_error, 0.25F);
    CY_CHECK_LT(runs[2].worst_error, 0.10F);
    CY_CHECK_LT(runs[2].worst_staleness, 0.2F);
}

CY_TEST_CASE("sky tables: the sky is a lookup — drawing it integrates no atmosphere") {
    const Atmosphere atmosphere = earth_atmosphere();
    const Vec3 ground = ground_position(atmosphere, 0.0F);

    IncrementalSkyView table;
    CY_REQUIRE(table.configure(SkyTableQuality::Low));
    CY_REQUIRE(table.update(atmosphere, ground, direction_at(35.0F, 20.0F), 0));

    const u64 before = table.stats().directions_integrated;
    Vec3 total{0.0F, 0.0F, 0.0F};
    for (u32 y = 0; y < 64; ++y) {
        for (u32 x = 0; x < 64; ++x) {
            const f32 elevation = ((static_cast<f32>(y) / 63.0F) * 180.0F) - 90.0F;
            const f32 azimuth = (static_cast<f32>(x) / 63.0F) * 360.0F;
            total = total + table.sample(direction_at(elevation, azimuth));
        }
    }
    // Four thousand pixels of sky, and not one of them integrated the atmosphere. That is the
    // requirement's "it SHALL sample tables rather than integrating the atmosphere per pixel",
    // counted rather than described.
    CY_CHECK_EQ(table.stats().directions_integrated, before);
    CY_CHECK_GT(total.y, 0.0F);
}

CY_TEST_CASE("sky tables: aerial perspective is the atmosphere's, in the engine's froxel volume") {
    const Atmosphere atmosphere = earth_atmosphere();
    AtmosphereTables tables;
    CY_REQUIRE(tables.configure(SkyTableQuality::Medium));
    CY_REQUIRE(tables.build(atmosphere));

    FroxelVolume volume;
    volume.width = 16;
    volume.height = 16;
    volume.depth = 32;
    volume.near_plane = 1.0F;
    volume.far_plane = 24000.0F;

    AerialPerspectiveTable aerial;
    CY_REQUIRE(aerial.configure(volume));
    CY_CHECK_FALSE(aerial.configure(FroxelVolume{0, 16, 32, 1.0F, 1000.0F, 2.0F}));
    CY_REQUIRE(aerial.configure(volume));

    const Vec3 eye = ground_position(atmosphere, 200.0F);
    AerialPerspectiveTable::View view;
    view.forward = Vec3{0.0F, 0.0F, -1.0F};
    view.right = Vec3{1.0F, 0.0F, 0.0F};
    view.up = Vec3{0.0F, 1.0F, 0.0F};
    const Vec3 sun = direction_at(30.0F, 10.0F);
    CY_REQUIRE(aerial.update(atmosphere, tables, eye, view, sun));
    CY_CHECK(aerial.built());
    CY_CHECK_EQ(aerial.froxels(), 16ULL * 16ULL * 32ULL);

    // The centre of the screen looks along -Z. What the table says about a surface eight kilometres
    // out must be what the atmosphere itself says, because the requirement forbids two models of
    // distance — and the table is only worth having if it agrees with the one it stands for.
    constexpr f32 kDistance = 8000.0F;
    const AerialPerspective sampled = aerial.sample(Vec2{0.5F, 0.5F}, kDistance);
    const AerialPerspective marched =
        aerial_perspective(atmosphere, eye, eye + (view.forward * kDistance), sun, 64);
    const f32 scattering_ratio =
        sampled.in_scattering.y / cy::math::max(marched.in_scattering.y, 1.0e-6F);
    CY_TEST_MESSAGE("aerial perspective at 8 km: transmittance ", sampled.transmittance.y,
                    " against ", marched.transmittance.y, ", in-scattering ",
                    sampled.in_scattering.y, " against ", marched.in_scattering.y, " (x",
                    scattering_ratio, ")");

    // TRANSMITTANCE IS PURE EXTINCTION, so the table and the march must agree closely: nothing in
    // the table's construction can make a path more or less opaque than the atmosphere says.
    CY_CHECK_LT(relative_difference(sampled.transmittance, marched.transmittance), 0.03F);

    // IN-SCATTERING IS NOT THE SAME QUANTITY IN THE TWO, AND THE GAP IS THE MEASUREMENT. M7's
    // `aerial_perspective()` integrates SINGLE scattering only; the table adds the tabulated
    // multiple scattering, which near the ground is a third of the light. So the table must be
    // brighter, by a factor that is a fraction rather than a multiple — a table that agreed exactly
    // would mean the multiple-scattering term was not reaching the aerial perspective at all, and
    // one that was five times brighter would mean it was reaching it with a missing 1/(4*pi).
    CY_CHECK_GT(scattering_ratio, 1.0F);
    CY_CHECK_LT(scattering_ratio, 2.5F);

    // Attenuation grows with distance and never runs backwards: a distance model that did would put
    // a bright band in the middle of a landscape.
    f32 previous = 2.0F;
    for (const f32 distance : {100.0F, 1000.0F, 4000.0F, 12000.0F, 22000.0F}) {
        const AerialPerspective step = aerial.sample(Vec2{0.5F, 0.5F}, distance);
        CY_CHECK_LE(step.transmittance.y, previous + 1.0e-4F);
        previous = step.transmittance.y;
    }

    // AND IT IS CONSISTENT WITH THE SKY ABOVE IT. "Aerial perspective SHALL be correct at large
    // scale, so that terrain kilometres away is attenuated consistently with the sky above it." At
    // the far end of the volume the in-scattering must be approaching the sky's own radiance in
    // that direction, because that is what an infinitely distant surface would show.
    const AerialPerspective far_end = aerial.sample(Vec2{0.5F, 0.5F}, volume.far_plane);
    const Vec3 sky = sky_radiance_tabulated(atmosphere, tables, eye, view.forward, sun, 48);
    const f32 fraction = far_end.in_scattering.y / cy::math::max(sky.y, 1.0e-6F);
    CY_TEST_MESSAGE("in-scattering at the far plane is ", fraction * 100.0F,
                    "% of the sky's radiance in the same direction");
    CY_CHECK_GT(fraction, 0.15F);
    CY_CHECK_LT(fraction, 1.05F);
}

CY_TEST_CASE("sky tables: the ground-to-orbit transition is one model, not a band per altitude") {
    const Atmosphere atmosphere = earth_atmosphere();
    AtmosphereTables tables;
    CY_REQUIRE(tables.configure(SkyTableQuality::Medium));
    CY_REQUIRE(tables.build(atmosphere));
    const Vec3 sun = direction_at(40.0F, 0.0F);

    // The horizon dips as the camera climbs, and it does so from the model: one arccosine, at every
    // altitude. At the ground it is zero; from 400 km it is about twenty degrees.
    CY_CHECK_NEAR(horizon_dip(atmosphere, 0.0F), 0.0F, 1.0e-6F);
    CY_CHECK_NEAR(horizon_dip(atmosphere, 400000.0F) * cy::math::kRadToDeg, 19.8F, 0.02F);

    f32 previous_dip = -1.0F;
    f32 previous_zenith = -1.0F;
    f32 ground_zenith = -1.0F;
    f32 worst_step = 0.0F;
    constexpr u32 kSteps = 80;
    for (u32 step = 0; step <= kSteps; ++step) {
        // Geometric in altitude, from two metres to four hundred kilometres, because a linear walk
        // would spend seventy-nine of its eighty samples in space.
        const f32 t = static_cast<f32>(step) / static_cast<f32>(kSteps);
        const f32 altitude = 2.0F * std::pow(200000.0F, t);
        const cy::world::WorldVec3d camera{123456.0, static_cast<double>(altitude), -98765.0};
        const PlanetaryView view = planetary_view(atmosphere, camera);

        // CAMERA-RELATIVE: the horizontal coordinates never reach the atmosphere, so the position
        // it integrates from has the magnitude of the planet's radius and nothing more. A model
        // that added the eye's absolute position would lose half a metre of precision per hundred
        // kilometres from the origin, and the symptom would be a sky that shimmers as you walk.
        // A relative bound of one part in a million: at Earth's radius an `f32` resolves about
        // half a metre, so anything looser would not notice a position built from the wrong
        // quantity.
        CY_CHECK_NEAR(length(view.planet_relative), atmosphere.planet_radius + altitude, 1.0e-6F);
        CY_CHECK_EQ(view.world_position.x, 123456.0F);

        const f32 dip = horizon_dip(atmosphere, altitude);
        CY_CHECK_GE(dip, previous_dip);
        previous_dip = dip;

        const Vec3 zenith = sky_radiance_tabulated(atmosphere, tables, view.planet_relative,
                                                   Vec3{0.0F, 1.0F, 0.0F}, sun, 24);
        if (ground_zenith < 0.0F) {
            ground_zenith = zenith.z;
        }
        if (previous_zenith >= 0.0F) {
            // Against the GROUND's radiance rather than against the previous sample. Above the
            // atmosphere's top the sky is exactly black, and a relative step measured against a
            // value approaching zero reports 100% for a difference of a hundredth of a nit — which
            // says nothing about whether the model has a seam in it.
            const f32 step_fraction =
                std::fabs(zenith.z - previous_zenith) / cy::math::max(ground_zenith, 1.0e-6F);
            worst_step = cy::math::max(worst_step, step_fraction);
        }
        previous_zenith = zenith.z;
    }

    // The transition to space is CONTINUOUS. There is no altitude band in this module and no
    // `if (altitude > ...)`, so the test that would catch one is a walk looking for a step — and
    // the sky at four hundred kilometres is nearly black, which is the model's answer rather than a
    // special case.
    CY_TEST_MESSAGE("ground to orbit: ground zenith ", ground_zenith,
                    " nits, worst step between samples ", worst_step * 100.0F,
                    "% of it, final zenith ", previous_zenith, " nits");
    CY_CHECK_LT(worst_step, 0.15F);
    // Above the atmosphere's top there is no atmosphere, so the sky is black — and it got there by
    // the model running out of air rather than by a band switching over.
    CY_CHECK_LT(previous_zenith, 1.0F);
    CY_CHECK_GT(ground_zenith, 100.0F);
}
