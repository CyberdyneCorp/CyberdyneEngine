// The water column's colour, underwater, reflections and caustics. M10 task 2.3, and `water`'s
// "Water surface shading", "Underwater rendering" and "Caustics" requirements.
//
// What is checked here is the part of shading that is PHYSICS rather than art: attenuation over a
// column, the body's own parameters reaching the underwater state, and the tier selections. The
// closure's evaluation on a device belongs to `rendering-materials-and-shading`, which this module
// does not link — see shading.h.

#include <cy/test/test.h>

#include <cy/water/shading.h>

#include "fixtures.h"

namespace test = cy::water::test;
using cy::water::CausticTier;
using cy::water::ReflectionBudget;
using cy::water::ReflectionSource;
using cy::water::RendererTier;
using cy::water::UnderwaterState;
using cy::water::WaterOptics;

CY_TEST_CASE("depth changes colour by attenuation, not by a painted gradient") {
    const WaterOptics sea = cy::water::clear_sea_optics();
    const cy::Vec3 sand{0.8F, 0.75F, 0.6F};

    const cy::Vec3 shallow = cy::water::water_column_colour(sea, 0.5F, sand);
    const cy::Vec3 deep = cy::water::water_column_colour(sea, 12.0F, sand);
    const cy::Vec3 abyss = cy::water::water_column_colour(sea, 60.0F, sand);

    // Darkens with depth, monotonically, on the channel the eye weights most.
    CY_CHECK_GT(shallow.y, deep.y);
    CY_CHECK_GT(deep.y, abyss.y);

    // AND SHIFTS HUE: red is absorbed some twenty-five times faster than blue, so what was a sandy
    // bottom becomes blue-green with depth. This is the specification's "without hand-authored
    // gradients" — nothing here was painted, it is three exponents.
    CY_CHECK_GT(shallow.x / shallow.z, deep.x / deep.z);
    CY_CHECK_LT(deep.x, deep.z);

    // Zero thickness is the background untouched: the column has to be crossed to change anything.
    const cy::Vec3 surface = cy::water::water_column_colour(sea, 0.0F, sand);
    CY_CHECK_NEAR(surface.x, sand.x, 1e-5F);
    CY_CHECK_NEAR(surface.z, sand.z, 1e-5F);
}

CY_TEST_CASE("two lakes look different underwater, because the parameters are the body's") {
    const WaterOptics sea = cy::water::clear_sea_optics();
    const WaterOptics silt = cy::water::silty_lake_optics();

    const UnderwaterState in_sea = cy::water::underwater_state(sea, 0.0, -6.0, 0.1F);
    const UnderwaterState in_lake = cy::water::underwater_state(silt, 0.0, -6.0, 0.1F);

    CY_CHECK(in_sea.camera_submerged);
    CY_CHECK(in_lake.camera_submerged);
    CY_CHECK_NEAR(in_sea.depth_metres, 6.0F, 1e-4F);

    // NOT A FIXED FULLSCREEN TINT: the silty lake extinguishes faster and scatters more, so its fog
    // is brighter and its caustics are weaker at the same depth.
    CY_CHECK_GT(in_lake.extinction.y, in_sea.extinction.y);
    CY_CHECK_GT(in_lake.fog_colour.x, in_sea.fog_colour.x);
    CY_CHECK_LT(in_lake.caustic_strength, in_sea.caustic_strength);
}

CY_TEST_CASE("crossing the surface is a fraction, not a switch") {
    const WaterOptics sea = cy::water::clear_sea_optics();
    // A camera whose near plane is 20 cm tall, rising through the surface.
    const cy::f32 radius = 0.2F;
    const UnderwaterState under = cy::water::underwater_state(sea, 0.0, -1.0, radius);
    const UnderwaterState half = cy::water::underwater_state(sea, 0.0, 0.0, radius);
    const UnderwaterState quarter = cy::water::underwater_state(sea, 0.0, 0.1F, radius);
    const UnderwaterState above = cy::water::underwater_state(sea, 0.0, 1.0, radius);

    CY_CHECK_NEAR(under.submerged_fraction, 1.0F, 1e-5F);
    CY_CHECK_NEAR(half.submerged_fraction, 0.5F, 1e-5F);
    CY_CHECK_NEAR(above.submerged_fraction, 0.0F, 1e-5F);
    // Monotone through the crossing, so the boundary is rendered explicitly rather than switching
    // between two full-screen states.
    CY_CHECK_LT(quarter.submerged_fraction, half.submerged_fraction);
    CY_CHECK_GT(quarter.submerged_fraction, above.submerged_fraction);
    CY_CHECK_FALSE(above.camera_submerged);
}

CY_TEST_CASE("foam roughens the closure and the reflectance comes from the index") {
    const WaterOptics sea = cy::water::clear_sea_optics();
    const cy::water::WaterSurfaceClosure clean =
        cy::water::build_closure(sea, cy::Vec3{0.0F, 1.0F, 0.0F}, 0.0F, 10.0F);
    const cy::water::WaterSurfaceClosure foamy =
        cy::water::build_closure(sea, cy::Vec3{0.0F, 1.0F, 0.0F}, 1.0F, 10.0F);

    CY_CHECK_NEAR(clean.roughness, sea.roughness, 1e-5F);
    CY_CHECK_GT(foamy.roughness, clean.roughness);
    // Schlick's F0 for water: ((1.333 - 1) / (1.333 + 1))^2 = 0.0204.
    CY_CHECK_NEAR(clean.f0, 0.0204F, 1e-3F);
    CY_CHECK_NEAR(clean.column_thickness, 10.0F, 1e-5F);
    CY_CHECK_NEAR(foamy.foam, 1.0F, 1e-5F);
}

CY_TEST_CASE("rough distant water resolves from cached radiance rather than tracing") {
    ReflectionBudget budget;
    // THE SCENARIO: "WHEN water is rough and distant THEN its reflection SHALL come from cached
    // radiance rather than dedicated rays."
    CY_CHECK_EQ(static_cast<int>(cy::water::select_reflection(budget, 0.4F, 600.0F)),
                static_cast<int>(ReflectionSource::CachedRadiance));
    CY_CHECK_EQ(static_cast<int>(cy::water::select_reflection(budget, 0.02F, 900.0F)),
                static_cast<int>(ReflectionSource::CachedRadiance));

    // Smooth and near: the hierarchy, cheapest first, escalating when a tier is unavailable.
    CY_CHECK_EQ(static_cast<int>(cy::water::select_reflection(budget, 0.01F, 20.0F)),
                static_cast<int>(ReflectionSource::ScreenTrace));
    ReflectionBudget no_screen = budget;
    no_screen.screen_tracing = false;
    CY_CHECK_EQ(static_cast<int>(cy::water::select_reflection(no_screen, 0.01F, 20.0F)),
                static_cast<int>(ReflectionSource::WorldTrace));
    ReflectionBudget hardware_only = budget;
    hardware_only.screen_tracing = false;
    hardware_only.world_tracing = false;
    hardware_only.hardware_tracing = true;
    CY_CHECK_EQ(static_cast<int>(cy::water::select_reflection(hardware_only, 0.01F, 20.0F)),
                static_cast<int>(ReflectionSource::HardwareTrace));
}

CY_TEST_CASE("the surface-derived caustic tier is the default for real-time use") {
    CY_CHECK_EQ(
        static_cast<int>(cy::water::select_caustic_tier(RendererTier::Standard, 0.3F, false)),
        static_cast<int>(CausticTier::SurfaceDerived));
    CY_CHECK_EQ(static_cast<int>(cy::water::select_caustic_tier(RendererTier::High, 0.3F, true)),
                static_cast<int>(CausticTier::SurfaceDerived));
    // A low profile, or a budget that will not pay for deriving one, falls back to the projected
    // approximation — which does NOT follow the surface, and that is why it is the last resort.
    CY_CHECK_EQ(static_cast<int>(cy::water::select_caustic_tier(RendererTier::Low, 0.9F, true)),
                static_cast<int>(CausticTier::Projected));
    CY_CHECK_EQ(static_cast<int>(cy::water::select_caustic_tier(RendererTier::High, 0.02F, true)),
                static_cast<int>(CausticTier::Projected));
    // And traced only where there is hardware for it and budget allocated to it.
    CY_CHECK_EQ(static_cast<int>(cy::water::select_caustic_tier(RendererTier::High, 0.8F, true)),
                static_cast<int>(CausticTier::Traced));
    CY_CHECK_EQ(static_cast<int>(cy::water::select_caustic_tier(RendererTier::High, 0.8F, false)),
                static_cast<int>(CausticTier::SurfaceDerived));
}
