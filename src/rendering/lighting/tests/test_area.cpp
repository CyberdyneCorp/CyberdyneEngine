// Area lights at unit cost: the closed-form cosine integral, the shape substitutions, the
// single-sided test and the documented fallback. Nothing here builds an `LtcTable`.
//
// Every case that needs one is in `test_reference.cpp` — building the table is 2.4 million BRDF
// evaluations, which is an integration-suite cost — and that is also where the Monte Carlo
// comparison lives that says what the moment-matched fit is WORTH.

#include <cy/test/test.h>

#include <cy/core/math/math.h>
#include <cy/rendering/lighting/area.h>

#include <cmath>

namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;
using cy::rendering::area_light_quad;
using cy::rendering::area_light_solid_angle;
using cy::rendering::AreaLight;
using cy::rendering::AreaLightShape;
using cy::rendering::AreaQuad;
using cy::rendering::emitter_filter_level;
using cy::rendering::integrate_cosine_polygon;
using cy::rendering::kRepresentativePointApproximation;
using cy::rendering::ltc_evaluate_diffuse;
using cy::rendering::representative_point;

/// A rect light of `half` metres, `distance` above the origin, facing down at it.
AreaLight overhead(f32 half, f32 distance) noexcept {
    AreaLight light;
    light.shape = AreaLightShape::Rect;
    light.center = Vec3{0.0F, 0.0F, distance};
    light.half_x = half;
    light.half_y = half;
    light.tangent_x = Vec3{1.0F, 0.0F, 0.0F};
    light.tangent_y = Vec3{0.0F, 1.0F, 0.0F};
    light.single_sided = false;
    return light;
}

}  // namespace

CY_TEST_CASE("area: the cosine polygon integral is normalised, signed and horizon clipped") {
    // A quad subtending the whole hemisphere integrates to 1. That single case pins the sign, the
    // orientation convention and the 1/(2 pi) normalisation at once — get any of the three wrong
    // and this number is negative, or 2, or 0.5.
    const f32 far_away = 1.0e4F;
    Vec3 hemisphere[4] = {Vec3{-far_away, -far_away, 0.001F}, Vec3{far_away, -far_away, 0.001F},
                          Vec3{far_away, far_away, 0.001F}, Vec3{-far_away, far_away, 0.001F}};
    CY_CHECK_NEAR(integrate_cosine_polygon(hemisphere, 4), 1.0F, 0.01F);

    // Reversing the winding reverses the sign, which the clamp turns into zero rather than into a
    // negative light.
    Vec3 reversed[4] = {hemisphere[3], hemisphere[2], hemisphere[1], hemisphere[0]};
    CY_CHECK_EQ(integrate_cosine_polygon(reversed, 4), 0.0F);

    // Entirely below the horizon: nothing, and in particular not a wrongly signed contribution.
    Vec3 below[4] = {Vec3{-1.0F, -1.0F, -1.0F}, Vec3{1.0F, -1.0F, -1.0F}, Vec3{1.0F, 1.0F, -1.0F},
                     Vec3{-1.0F, 1.0F, -1.0F}};
    CY_CHECK_EQ(integrate_cosine_polygon(below, 4), 0.0F);
}

CY_TEST_CASE("area: a light sinking below the horizon fades to zero rather than brightening") {
    // The failure the horizon clip exists to prevent: without it, the corners below the surface
    // contribute with the wrong sign and a light setting behind a floor gets BRIGHTER as it goes.
    f32 previous = 1.0F;
    for (u32 step = 0; step <= 20; ++step) {
        AreaLight light = overhead(1.0F, 0.0F);
        // Rotate the light down past the horizon, from straight overhead to well below.
        const f32 angle = cy::math::kPi * static_cast<f32>(step) / 20.0F;
        light.center = Vec3{std::sin(angle) * 2.0F, 0.0F, std::cos(angle) * 2.0F};
        const f32 value = ltc_evaluate_diffuse(light, Vec3{0.0F, 0.0F, 1.0F});
        CY_CHECK_LE(value, previous + 1.0e-4F);
        previous = value;
    }
    CY_CHECK_NEAR(previous, 0.0F, 1.0e-3F);
}

CY_TEST_CASE("area: a disc and a rect of the same emitting area read as the same brightness") {
    // The equal-AREA substitution, not the inscribed one. Getting it wrong is a 21% brightness
    // difference between a disc light and a rect light an artist thought were the same.
    AreaLight rect = overhead(1.0F, 3.0F);
    AreaLight disc = overhead(1.0F, 3.0F);
    disc.shape = AreaLightShape::Disc;
    // A disc of radius r has area pi r^2; the equal-area rect has half-extent r sqrt(pi)/2.
    rect.half_x = 1.0F * 0.8862269F;
    rect.half_y = 1.0F * 0.8862269F;

    const Vec3 normal{0.0F, 0.0F, 1.0F};
    CY_CHECK_NEAR(ltc_evaluate_diffuse(disc, normal), ltc_evaluate_diffuse(rect, normal), 1.0e-4F);
}


CY_TEST_CASE("area: the representative-point fallback is documented and widens with the light") {
    // "WHEN the shading model does not support LTC (hair, cloth) THEN the area light SHALL be
    // approximated by a representative point with a documented approximation."
    CY_REQUIRE(kRepresentativePointApproximation != nullptr);
    CY_CHECK(kRepresentativePointApproximation[0] != '\0');

    const Vec3 normal{0.0F, 0.0F, 1.0F};
    const Vec3 view{0.0F, 0.0F, 1.0F};

    const auto small = representative_point(overhead(0.05F, 3.0F), normal, view, 0.15F);
    const auto large = representative_point(overhead(1.50F, 3.0F), normal, view, 0.15F);

    // A bigger light gives a broader highlight...
    CY_CHECK_GT(large.effective_roughness, small.effective_roughness);
    CY_CHECK_GE(small.effective_roughness, 0.15F - 1.0e-3F);
    // ...and the energy normalisation goes with it, so the highlight spreads without brightening.
    // That second half is the part this approximation is famous for getting wrong.
    CY_CHECK_LT(large.energy_scale, small.energy_scale);
    CY_CHECK_LE(large.energy_scale, 1.0F);

    // The point is on the emitter, not at its centre, whenever the reflection ray misses the
    // centre — that is the whole of what makes the highlight land in the right place.
    const Vec3 grazing = normalize(Vec3{0.8F, 0.0F, 0.6F});
    const auto offset = representative_point(overhead(1.5F, 3.0F), normal, grazing, 0.15F);
    CY_CHECK_GT(std::fabs(offset.position.x), 0.01F);
}

CY_TEST_CASE("area: a textured emitter is filtered by roughness and by how much sky it fills") {
    constexpr u32 kMips = 9;
    const AreaLight small = overhead(0.1F, 5.0F);
    const AreaLight huge = overhead(20.0F, 1.0F);

    // A mirror looking at a small light reads texels.
    CY_CHECK_NEAR(emitter_filter_level(0.0F, area_light_solid_angle(small), kMips), 0.0F, 0.05F);
    // A rough surface reads the emitter's average colour whatever the light's size.
    CY_CHECK_NEAR(emitter_filter_level(1.0F, area_light_solid_angle(small), kMips), 8.0F, 0.05F);
    // And so does a mirror when the emitter fills the view: the footprint is large either way.
    CY_CHECK_GT(emitter_filter_level(0.0F, area_light_solid_angle(huge), kMips), 6.0F);

    CY_CHECK_EQ(emitter_filter_level(0.5F, 1.0F, 0), 0.0F);
}

CY_TEST_CASE("area: a single-sided emitter facing away contributes nothing") {
    AreaLight light = overhead(1.0F, 3.0F);
    light.single_sided = true;
    // tangent_x cross tangent_y points +Z, which is AWAY from the shading point at the origin.
    CY_CHECK_EQ(ltc_evaluate_diffuse(light, Vec3{0.0F, 0.0F, 1.0F}), 0.0F);

    // Flipped so the emitter faces down at the surface, it lights it.
    light.tangent_y = Vec3{0.0F, -1.0F, 0.0F};
    CY_CHECK_GT(ltc_evaluate_diffuse(light, Vec3{0.0F, 0.0F, 1.0F}), 0.0F);
}

CY_TEST_CASE("area: a sphere light faces the shading point however it is turned") {
    AreaLight sphere = overhead(0.5F, 4.0F);
    sphere.shape = AreaLightShape::Sphere;
    const f32 upright = ltc_evaluate_diffuse(sphere, Vec3{0.0F, 0.0F, 1.0F});
    sphere.tangent_x = normalize(Vec3{0.3F, 0.9F, 0.2F});
    sphere.tangent_y = normalize(cross(Vec3{0.0F, 0.0F, 1.0F}, sphere.tangent_x));
    const f32 turned = ltc_evaluate_diffuse(sphere, Vec3{0.0F, 0.0F, 1.0F});
    CY_CHECK_NEAR(upright, turned, 1.0e-4F);
    CY_CHECK_GT(upright, 0.0F);

    const AreaQuad quad = area_light_quad(sphere);
    for (u32 index = 0; index < 4; ++index) {
        CY_CHECK_GT(quad.corner[index].z, 0.0F);
    }
}
