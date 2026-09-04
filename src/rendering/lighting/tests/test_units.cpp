// Physical light units, colour temperature, exposure, and the GPU light record. Task 4.4.1.
//
// These are the cases that make "physical units" a set of numbers rather than a claim. Each one is
// checkable against a physical fact somebody can look up, which is the point: an engine whose
// lighting units are self-consistent but not real is an engine where every scene is tuned twice.

#include <cy/test/test.h>

#include <cy/core/math/scalar.h>
#include <cy/rendering/lighting/lights.h>
#include <cy/rendering/lighting/units.h>

#include <cmath>
#include <numbers>

namespace {

using cy::rendering::CameraExposure;
using cy::rendering::PhotometricUnit;

}  // namespace

CY_TEST_CASE("a bulb's lumens become the candela the shader integrates") {
    // A 1000 lm omnidirectional bulb is 1000 / 4π ≈ 79.6 cd. That is a fact about the world, not
    // about this engine.
    const cy::f32 candela = cy::rendering::point_candela_from_lumens(1000.0F);
    CY_CHECK_NEAR(candela, 79.577F, 0.01F);

    CY_CHECK_NEAR(cy::rendering::to_shading_intensity(cy::render::LightKind::Point,
                                                      PhotometricUnit::Lumen, 1000.0F, 0.0F, 0.0F),
                  candela, 1e-3F);
    // Already in the shading unit: passed through, because converting a candela value again is how
    // a light ends up 4π times too dim.
    CY_CHECK_EQ(cy::rendering::to_shading_intensity(cy::render::LightKind::Point,
                                                    PhotometricUnit::Candela, 42.0F, 0.0F, 0.0F),
                42.0F);
}

CY_TEST_CASE("the spot convention is the photometric one, and the physical one is available") {
    // THE ONE REAL CHOICE units.h makes. Under the photometric convention a spot's intensity does
    // not depend on its cone: narrowing the beam shrinks the lit region and leaves its brightness
    // alone, which is what an artist means by narrowing a beam.
    const cy::f32 narrow = cy::rendering::to_shading_intensity(
        cy::render::LightKind::Spot, PhotometricUnit::Lumen, 1000.0F, 0.1F, 0.0F);
    const cy::f32 wide = cy::rendering::to_shading_intensity(
        cy::render::LightKind::Spot, PhotometricUnit::Lumen, 1000.0F, 1.0F, 0.0F);
    CY_CHECK_EQ(narrow, wide);
    CY_CHECK_NEAR(narrow, 1000.0F / cy::rendering::kPi, 0.01F);

    // The physical alternative does depend on the cone, and by a large factor — which is why the
    // two are separate functions rather than one with a flag somebody forgets to set.
    CY_CHECK_GT(cy::rendering::spot_candela_physical(1000.0F, 0.1F),
                cy::rendering::spot_candela_physical(1000.0F, 1.0F) * 10.0F);
}

CY_TEST_CASE("a colour temperature is a tint, not a second intensity") {
    // Every temperature comes back at unit luminance, so changing a light's temperature changes its
    // colour and leaves its brightness alone.
    for (const cy::f32 kelvin : {2000.0F, 3200.0F, 6500.0F, 10000.0F}) {
        const cy::Vec3 tint = cy::rendering::blackbody_color(kelvin);
        const cy::f32 luminance = (0.2126F * tint.x) + (0.7152F * tint.y) + (0.0722F * tint.z);
        CY_CHECK_NEAR(luminance, 1.0F, 1e-3F);
    }

    // Warm is redder than blue; cool is the other way round. The direction, which is the thing a
    // transcription error in the locus would reverse.
    const cy::Vec3 warm = cy::rendering::blackbody_color(2000.0F);
    const cy::Vec3 cool = cy::rendering::blackbody_color(10000.0F);
    CY_CHECK_GT(warm.x, warm.z);
    CY_CHECK_GT(cool.z, cool.x);

    // 6500 K is the sRGB white point, so it is close to neutral.
    const cy::Vec3 neutral = cy::rendering::blackbody_color(6500.0F);
    CY_CHECK_NEAR(neutral.x, 1.0F, 0.05F);
    CY_CHECK_NEAR(neutral.z, 1.0F, 0.08F);
}

CY_TEST_CASE("exposure is a photographer's, and the sunny 16 rule lands where it should") {
    // f/16, 1/100 s, ISO 100 is the "sunny 16" exposure for direct daylight, which is EV 15.
    CameraExposure sunny;
    sunny.aperture = 16.0F;
    sunny.shutter_seconds = 1.0F / 100.0F;
    sunny.sensitivity = 100.0F;
    CY_CHECK_NEAR(cy::rendering::exposure_value(sunny), 15.0F, 0.05F);

    // Opening one stop (f/16 to f/11.3) lowers EV by one, and doubling the ISO lowers it by one.
    CameraExposure open = sunny;
    open.aperture = 16.0F / std::numbers::sqrt2_v<cy::f32>;
    CY_CHECK_NEAR(cy::rendering::exposure_value(open), 14.0F, 0.05F);
    CameraExposure fast = sunny;
    fast.sensitivity = 200.0F;
    CY_CHECK_NEAR(cy::rendering::exposure_value(fast), 14.0F, 0.05F);

    // Positive compensation lowers EV and therefore brightens, which is the sign photographers use.
    CameraExposure compensated = sunny;
    compensated.compensation = 1.0F;
    CY_CHECK_LT(cy::rendering::exposure_value(compensated), cy::rendering::exposure_value(sunny));
    CY_CHECK_GT(cy::rendering::exposure_multiplier(compensated),
                cy::rendering::exposure_multiplier(sunny));
}

CY_TEST_CASE("a physically-configured scene exposes without per-light tuning") {
    // The specification's own scenario: 100 000 lux sunlight and an EV-based camera. A Lambertian
    // surface of albedo 0.18 under 100 000 lux has luminance 0.18 · 100000 / π ≈ 5730 nits; exposed
    // at the EV that meters it, the result must land near mid grey rather than at 0 or 20.
    const cy::f32 luminance = 0.18F * 100000.0F / cy::rendering::kPi;
    const cy::f32 metered = cy::rendering::exposure_value_for_luminance(luminance);
    const cy::f32 exposed = luminance * cy::rendering::exposure_multiplier(metered);
    CY_CHECK_GT(exposed, 0.05F);
    CY_CHECK_LT(exposed, 0.5F);

    // And the metered EV for daylight is in the neighbourhood the sunny 16 rule names, which is
    // what says the two halves — the light's units and the camera's — are on the same scale.
    CY_CHECK_GT(metered, 13.0F);
    CY_CHECK_LT(metered, 17.0F);
}

CY_TEST_CASE(
    "switching from arbitrary units is documented and reversible, and flags the implausible") {
    const cy::f32 physical =
        cy::rendering::arbitrary_to_physical(cy::render::LightKind::Point, 1.0F);
    CY_CHECK(cy::rendering::intensity_is_plausible(cy::render::LightKind::Point, physical));
    CY_CHECK_NEAR(cy::rendering::physical_to_arbitrary(cy::render::LightKind::Point, physical),
                  1.0F, 1e-4F);

    // A value that is not a light: flagged rather than clamped, because clamping would hide the
    // content error the flag exists to report.
    CY_CHECK_FALSE(cy::rendering::intensity_is_plausible(cy::render::LightKind::Point, 1e9F));
    CY_CHECK_FALSE(cy::rendering::intensity_is_plausible(cy::render::LightKind::Directional, 0.0F));
    CY_CHECK(cy::rendering::intensity_is_plausible(cy::render::LightKind::Directional, 100000.0F));
}

CY_TEST_CASE("a light a million units out reaches the GPU as a small, exact number") {
    // design.md §3, at the moment the struct is written rather than when precision breaks. The
    // subtraction happens in f64 on the CPU, so the difference is exact and only the result — a
    // small number — is rounded to f32.
    cy::render::LightDescription light;
    light.kind = cy::render::LightKind::Point;
    light.transform.translation = cy::Vec3{1000000.5F, 0.0F, 0.0F};
    light.intensity = 1000.0F;
    light.range = 10.0F;

    cy::rendering::LightBuildParameters parameters;
    parameters.unit = PhotometricUnit::Lumen;
    parameters.camera_origin = cy::Vec3{1000000.0F, 0.0F, 0.0F};

    const cy::rendering::GpuLight record = cy::rendering::build_gpu_light(light, parameters);
    CY_CHECK_NEAR(record.position_relative_to_camera[0], 0.5F, 1e-5F);
    CY_CHECK_EQ(record.kind, cy::rendering::kGpuLightPoint);
    CY_CHECK_NEAR(record.intensity, cy::rendering::point_candela_from_lumens(1000.0F), 1e-3F);
    CY_CHECK_EQ(record.shadow_slot, cy::rendering::kNoShadowSlot);
}

CY_TEST_CASE("a non-spot's cone terms evaluate to one, and a spot's fall off inside its cone") {
    cy::render::LightDescription point;
    point.kind = cy::render::LightKind::Point;
    const cy::rendering::GpuLight point_record =
        cy::rendering::build_gpu_light(point, cy::rendering::LightBuildParameters{});
    // `saturate(cos · scale + bias)` must be 1 for every direction, which `scale = 0, bias = 1` is.
    CY_CHECK_EQ(point_record.spot_scale, 0.0F);
    CY_CHECK_EQ(point_record.spot_bias, 1.0F);

    cy::render::LightDescription spot;
    spot.kind = cy::render::LightKind::Spot;
    spot.inner_cone_radians = 0.3F;
    spot.outer_cone_radians = 0.6F;
    const cy::rendering::GpuLight spot_record =
        cy::rendering::build_gpu_light(spot, cy::rendering::LightBuildParameters{});

    const auto cone = [&](cy::f32 angle) {
        const cy::f32 value = (std::cos(angle) * spot_record.spot_scale) + spot_record.spot_bias;
        return cy::math::clamp(value, 0.0F, 1.0F);
    };
    CY_CHECK_NEAR(cone(0.0F), 1.0F, 1e-4F);  // on the axis
    CY_CHECK_NEAR(cone(0.3F), 1.0F, 1e-4F);  // at the inner cone
    CY_CHECK_NEAR(cone(0.6F), 0.0F, 1e-4F);  // at the outer cone
    CY_CHECK_GT(cone(0.45F), 0.0F);          // between them
    CY_CHECK_LT(cone(0.45F), 1.0F);
}

CY_TEST_CASE("light importance ranks by coverage times intensity, deterministically") {
    cy::render::LightDescription near_light;
    near_light.kind = cy::render::LightKind::Point;
    near_light.range = 10.0F;
    near_light.transform.translation = cy::Vec3{0.0F, 0.0F, -5.0F};

    cy::render::LightDescription far_light = near_light;
    far_light.transform.translation = cy::Vec3{0.0F, 0.0F, -500.0F};

    const cy::Vec3 camera{0.0F, 0.0F, 0.0F};
    CY_CHECK_GT(cy::rendering::light_importance(near_light, camera, 1000.0F),
                cy::rendering::light_importance(far_light, camera, 1000.0F));

    // A directional light covers the whole view: its importance is its intensity, and it is never
    // the one that gets dropped.
    cy::render::LightDescription sun;
    sun.kind = cy::render::LightKind::Directional;
    CY_CHECK_EQ(cy::rendering::light_importance(sun, camera, 100000.0F), 100000.0F);
}
