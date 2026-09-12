// The ocean: a spectrum driven by wind and fetch, and a surface generated around the camera rather
// than stored for the world. M10 task 2.3, and `water`'s "Ocean simulation" requirement.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL — reported in `verified_failing`: `OceanSurface::
// build()`'s origin snap was replaced by the camera position itself, so the lattice moved with the
// camera by fractions of a cell. "the patch is generated around the camera and snapped, not
// swimming under it" went red. The snap was then restored.

#include <cy/test/test.h>

#include <cy/water/ocean.h>

#include <algorithm>
#include <cmath>

#include "fixtures.h"

namespace test = cy::water::test;
using cy::water::BandAuthority;
using cy::water::BandSelection;
using cy::water::DisplacementModel;
using cy::water::OceanParams;
using cy::water::OceanReport;
using cy::water::OceanSurface;
using cy::water::OceanSurfaceParams;

CY_TEST_CASE(
    "the spectrum is cascaded: several bands over a wavelength range, not one simulation") {
    OceanReport report;
    const auto model = cy::water::build_ocean_model(test::rough_sea(), 99, report);
    CY_REQUIRE(model.has_value());

    CY_CHECK_EQ(model->band_count, 4u);
    CY_CHECK_EQ(report.cascades, 4u);
    // The cascades tile the declared range, each covering a wavelength range, coarsest last.
    CY_CHECK_NEAR(model->bands[0].wavelength_min, test::rough_sea().shortest_wavelength, 1e-3F);
    CY_CHECK_NEAR(model->bands[3].wavelength_max, test::rough_sea().longest_wavelength, 1e-3F);
    for (cy::u32 index = 1; index < model->band_count; ++index) {
        CY_CHECK_NEAR(model->bands[index].wavelength_min, model->bands[index - 1].wavelength_max,
                      1e-3F);
    }

    // A ten metre per second wind over three hundred kilometres is a rough sea: significant wave
    // height between one and four metres is what a forecast would say, and what this reports.
    CY_CHECK_GT(report.significant_height_metres, 1.0F);
    CY_CHECK_LT(report.significant_height_metres, 4.0F);
    // And the peak of the spectrum is a swell wavelength, tens of metres rather than centimetres.
    CY_CHECK_GT(report.peak_wavelength_metres, 20.0F);
}

CY_TEST_CASE("wind changes the sea, consistently for rendering and for authoritative queries") {
    OceanReport calm_report;
    OceanParams calm = test::rough_sea();
    calm.wind_speed_mps = 4.0F;
    const auto calm_model = cy::water::build_ocean_model(calm, 7, calm_report);
    CY_REQUIRE(calm_model.has_value());

    OceanReport storm_report;
    OceanParams storm = test::rough_sea();
    storm.wind_speed_mps = 18.0F;
    const auto storm_model = cy::water::build_ocean_model(storm, 7, storm_report);
    CY_REQUIRE(storm_model.has_value());

    // THE SCENARIO: "WHEN the wind field's strength increases THEN the spectrum SHALL change and
    // the sea state SHALL follow". Nothing was re-authored between these two models.
    CY_CHECK_GT(storm_report.significant_height_metres, calm_report.significant_height_metres);
    CY_CHECK_GT(storm_report.peak_wavelength_metres, calm_report.peak_wavelength_metres);

    // "consistently for rendering and for authoritative queries": both selections rise.
    cy::f64 calm_range = 0.0;
    cy::f64 storm_range = 0.0;
    for (cy::u32 step = 0; step < 48; ++step) {
        const cy::f64 x = static_cast<cy::f64>(step) * 3.0;
        calm_range = std::max(
            calm_range,
            std::fabs(evaluate_displacement(*calm_model, BandSelection::Authoritative, x, 0.0, 0.0)
                          .height));
        storm_range = std::max(
            storm_range,
            std::fabs(evaluate_displacement(*storm_model, BandSelection::Authoritative, x, 0.0, 0.0)
                          .height));
    }
    CY_CHECK_GT(storm_range, calm_range);
}

CY_TEST_CASE("a short fetch chops and a long fetch swells") {
    OceanReport bay_report;
    OceanParams bay = test::rough_sea();
    bay.fetch_km = 2.0F;
    CY_REQUIRE(cy::water::build_ocean_model(bay, 3, bay_report).has_value());

    OceanReport open_report;
    OceanParams open = test::rough_sea();
    open.fetch_km = 500.0F;
    CY_REQUIRE(cy::water::build_ocean_model(open, 3, open_report).has_value());

    // Same wind, different fetch: the sheltered water carries shorter, lower waves. A sea state
    // that ignored fetch would report the same height for both.
    CY_CHECK_LT(bay_report.significant_height_metres, open_report.significant_height_metres);
    CY_CHECK_LT(bay_report.peak_wavelength_metres, open_report.peak_wavelength_metres);
}

CY_TEST_CASE("capillary cascades are visual, and a cascade that would be felt is promoted") {
    OceanReport calm_report;
    OceanParams calm = test::rough_sea();
    calm.wind_speed_mps = 5.0F;
    const auto calm_model = cy::water::build_ocean_model(calm, 11, calm_report);
    CY_REQUIRE(calm_model.has_value());
    // The short cascade is declared visual, and its amplitude is below the felt threshold.
    CY_CHECK_EQ(static_cast<int>(calm_model->bands[0].authority),
                static_cast<int>(BandAuthority::Visual));
    CY_CHECK_EQ(calm_report.promoted_cascades, 0u);
    CY_CHECK_EQ(static_cast<int>(cy::water::validate_model(*calm_model)),
                static_cast<int>(cy::water::DisplacementProblem::None));

    OceanReport storm_report;
    OceanParams storm = test::rough_sea();
    storm.wind_speed_mps = 30.0F;
    storm.visual_wavelength_metres = 12.0F;  // a wide visual band, so the storm fills it
    const auto storm_model = cy::water::build_ocean_model(storm, 11, storm_report);
    CY_REQUIRE(storm_model.has_value());
    // THE PROMOTION: rather than emit a model whose visual band is large enough to be felt — which
    // the specification calls a violation — the builder makes it authoritative and says so.
    CY_CHECK_GT(storm_report.promoted_cascades, 0u);
    CY_CHECK_EQ(static_cast<int>(cy::water::validate_model(*storm_model)),
                static_cast<int>(cy::water::DisplacementProblem::None));
    CY_CHECK_LE(storm_report.split.largest_visual_band_metres, storm.felt_threshold_metres);
}

CY_TEST_CASE("parameters that cannot describe a sea are refused") {
    OceanParams dead = test::rough_sea();
    dead.wind_speed_mps = 0.0F;
    CY_CHECK_FALSE(cy::water::build_ocean_model(dead, 1).has_value());

    OceanParams inverted = test::rough_sea();
    inverted.longest_wavelength = 0.1F;
    CY_CHECK_FALSE(cy::water::build_ocean_model(inverted, 1).has_value());

    OceanParams crowded = test::rough_sea();
    crowded.cascades = cy::water::kMaxDisplacementBands + 1;
    CY_CHECK_FALSE(cy::water::build_ocean_model(crowded, 1).has_value());
}

CY_TEST_CASE("no world-scale ocean mesh exists: the patch costs the same wherever the camera is") {
    const auto model = cy::water::build_ocean_model(test::rough_sea(), 5);
    CY_REQUIRE(model.has_value());

    OceanSurface surface(test::allocator());
    OceanSurfaceParams params;
    params.near_cell_metres = 2.0F;
    params.ring_quads = 8;
    params.rings = 3;
    CY_REQUIRE(surface.configure(params).has_value());

    CY_REQUIRE(surface.build(*model, cy::world::WorldVec3d{0.0, 0.0, 0.0}, 0.0).has_value());
    const cy::usize vertices_at_origin = surface.vertex_count();
    const cy::u64 bytes_at_origin = surface.bytes();
    CY_CHECK_GT(vertices_at_origin, 0u);

    // A THOUSAND KILOMETRES OUT. An ocean stored as a world-scale mesh would need a mesh a thousand
    // kilometres wide to answer this; a camera-relative one costs exactly what it cost at the
    // origin, and this equality is the requirement measured rather than asserted.
    CY_REQUIRE(surface.build(*model, cy::world::WorldVec3d{1.0e6, 0.0, -7.5e5}, 12.0).has_value());
    CY_CHECK_EQ(surface.vertex_count(), vertices_at_origin);
    CY_CHECK_EQ(surface.bytes(), bytes_at_origin);

    // And the patch reaches as far as its rings say and no further, whatever the ocean's bounds.
    CY_CHECK_NEAR(surface.extent_metres(), 2.0F * 4.0F * 8.0F * 0.5F, 1e-4F);
}

CY_TEST_CASE("the patch is generated around the camera and snapped, not swimming under it") {
    const auto model = cy::water::build_ocean_model(test::rough_sea(), 5);
    CY_REQUIRE(model.has_value());
    OceanSurface surface(test::allocator());
    OceanSurfaceParams params;
    // A small patch: what this case measures is where the lattice lands, not how much of it there
    // is, and a unit test has a millisecond.
    params.near_cell_metres = 2.0F;
    params.ring_quads = 4;
    params.rings = 2;
    CY_REQUIRE(surface.configure(params).has_value());

    // The coarsest ring's cell is 2 * 2 = 4 m, so the origin snaps to multiples of four.
    CY_REQUIRE(surface.build(*model, cy::world::WorldVec3d{101.0, 0.0, 49.0}, 0.0).has_value());
    const cy::world::WorldVec3d first = surface.origin();
    CY_CHECK_NEAR(static_cast<cy::f32>(std::fmod(first.x, 4.0)), 0.0F, 1e-4F);
    CY_CHECK_NEAR(static_cast<cy::f32>(std::fmod(first.z, 4.0)), 0.0F, 1e-4F);

    // A sub-cell camera move does not move the lattice: the patch slides in whole cells or not at
    // all, which is what keeps the surface from shimmering as the camera drifts.
    CY_REQUIRE(surface.build(*model, cy::world::WorldVec3d{102.0, 0.0, 50.0}, 0.0).has_value());
    CY_CHECK_EQ(surface.origin().x, first.x);
    CY_CHECK_EQ(surface.origin().z, first.z);

    // A move of several coarse cells does.
    CY_REQUIRE(surface.build(*model, cy::world::WorldVec3d{131.0, 0.0, 49.0}, 0.0).has_value());
    CY_CHECK_NE(surface.origin().x, first.x);
}

CY_TEST_CASE("the patch's vertices are the rendering evaluation of the same model") {
    const auto model = cy::water::build_ocean_model(test::rough_sea(), 17);
    CY_REQUIRE(model.has_value());
    OceanSurface surface(test::allocator());
    OceanSurfaceParams params;
    params.near_cell_metres = 4.0F;
    params.ring_quads = 4;
    params.rings = 1;
    CY_REQUIRE(surface.configure(params).has_value());
    CY_REQUIRE(surface.build(*model, cy::world::WorldVec3d{64.0, 0.0, 64.0}, 2.5).has_value());

    // The centre vertex of a 4x4 ring is index 12 of a 5x5 grid: (2, 2), which is exactly the
    // origin. Its height must be the model's rendering answer there — the patch is a sampling of
    // `evaluate_displacement`, not a second surface.
    const cy::Vec3 centre = surface.positions()[12];
    const cy::water::Displacement expected = evaluate_displacement(
        *model, BandSelection::All, surface.origin().x, surface.origin().z, 2.5);
    CY_CHECK_NEAR(centre.y, static_cast<cy::f32>(expected.height - surface.origin().y), 1e-4F);
    CY_CHECK_NEAR(centre.x, static_cast<cy::f32>(expected.offset.x), 1e-4F);

    // And it carries the breaking indicator the foam generator reads.
    CY_CHECK_EQ(surface.breaking().size(), surface.positions().size());
    CY_CHECK_EQ(surface.normals().size(), surface.positions().size());
    CY_CHECK_GT(surface.triangle_count(), 0u);
}

CY_TEST_CASE("a patch shape that cannot be built is refused") {
    OceanSurface surface(test::allocator());
    OceanSurfaceParams odd;
    odd.ring_quads = 7;
    CY_CHECK_FALSE(surface.configure(odd).has_value());

    OceanSurfaceParams ringless;
    ringless.rings = 0;
    CY_CHECK_FALSE(surface.configure(ringless).has_value());

    // And an unconfigured patch refuses to build rather than producing an empty one.
    const auto model = cy::water::build_ocean_model(test::rough_sea(), 5);
    CY_REQUIRE(model.has_value());
    OceanSurface fresh(test::allocator());
    CY_CHECK_FALSE(fresh.build(*model, cy::world::WorldVec3d{}, 0.0).has_value());
}
