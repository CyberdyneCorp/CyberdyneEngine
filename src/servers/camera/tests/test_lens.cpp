// The lens model: the two authoring forms, and the interpolation rule that is the whole point of
// having them. Task 4.3.2.
//
// `camera-system` — "Lens model": "lens blending SHALL respect the lens model in use, so that a
// physical lens does not interpolate as though it were a field-of-view value." That sentence is a
// number, and these cases are the number: half way between an 18 mm and a 50 mm lens is 34 mm and
// 38.88 degrees, not the 47.19 degrees a field-of-view lerp would give. A blend written the obvious
// way passes every "does it look about right" check and fails these.

#include <cy/core/math/scalar.h>
#include <cy/servers/camera/lens.h>
#include <cy/test/test.h>

#include <cmath>

using cy::f32;
using namespace cy::camera;

namespace {

constexpr f32 kSensor = 24.0F;

[[nodiscard]] Lens physical(f32 focal_length_mm) noexcept {
    Lens lens;
    lens.kind = LensKind::Physical;
    lens.physical.focal_length_mm = focal_length_mm;
    lens.physical.sensor_height_mm = kSensor;
    return lens;
}

[[nodiscard]] Lens gameplay(f32 fov_degrees) noexcept {
    Lens lens;
    lens.kind = LensKind::Gameplay;
    lens.gameplay.vertical_fov_radians = fov_degrees * cy::math::kDegToRad;
    return lens;
}

[[nodiscard]] f32 degrees(f32 radians) noexcept {
    return radians * cy::math::kRadToDeg;
}

}  // namespace

CY_TEST_CASE("focal length and field of view are each other's inverse") {
    for (const f32 focal : {14.0F, 24.0F, 35.0F, 50.0F, 135.0F}) {
        const f32 fov = fov_from_focal_length(focal, kSensor);
        CY_CHECK_NEAR(focal_length_from_fov(fov, kSensor), focal, 1e-3F);
    }
}

CY_TEST_CASE("a physical lens reports the field of view its focal length implies") {
    CY_CHECK_NEAR(degrees(physical(18.0F).vertical_fov_radians()), 67.380F, 1e-2F);
    CY_CHECK_NEAR(degrees(physical(50.0F).vertical_fov_radians()), 26.991F, 1e-2F);
}

CY_TEST_CASE("two physical lenses interpolate in focal length, not in field of view") {
    // THE REQUIREMENT, AS A NUMBER. See the header comment: an operator racking a zoom moves the
    // barrel, so the midpoint is 34 mm — 38.88 degrees. A field-of-view lerp would give 47.19
    // degrees, which is 27.48 mm, and would look like a much faster zoom at the wide end.
    const Lens mid = blend(physical(18.0F), physical(50.0F), 0.5F);
    CY_CHECK_EQ(mid.kind, LensKind::Physical);
    CY_CHECK_NEAR(mid.physical.focal_length_mm, 34.0F, 1e-3F);
    CY_CHECK_NEAR(degrees(mid.vertical_fov_radians()), 38.880F, 1e-2F);
    // And it is NOT the field-of-view midpoint, which is the assertion that fails if somebody
    // "simplifies" the blend.
    CY_CHECK_GT(std::fabs(degrees(mid.vertical_fov_radians()) - 47.186F), 8.0F);
}

CY_TEST_CASE("the gameplay half of a blended physical lens is kept in step") {
    const Lens mid = blend(physical(18.0F), physical(50.0F), 0.5F);
    // A consumer reading `gameplay.vertical_fov_radians` directly must not see a stale value from
    // before the blend.
    CY_CHECK_NEAR(mid.gameplay.vertical_fov_radians, mid.vertical_fov_radians(), 1e-5F);
}

CY_TEST_CASE("two gameplay lenses interpolate in field of view") {
    const Lens mid = blend(gameplay(40.0F), gameplay(80.0F), 0.5F);
    CY_CHECK_EQ(mid.kind, LensKind::Gameplay);
    CY_CHECK_NEAR(degrees(mid.vertical_fov_radians()), 60.0F, 1e-3F);
}

CY_TEST_CASE("a gameplay lens blended into a physical one interpolates in field of view") {
    // The mixed case: focal length is not a quantity the gameplay lens authored, so blending in it
    // would mean inventing a sensor for the gameplay end.
    const Lens mid = blend(gameplay(60.0F), physical(50.0F), 0.5F);
    CY_CHECK_NEAR(degrees(mid.vertical_fov_radians()), (60.0F + 26.991F) * 0.5F, 1e-2F);
}

CY_TEST_CASE("the projection kind switches at the halfway point rather than interpolating") {
    Lens ortho;
    ortho.projection = cy::render::ProjectionKind::Orthographic;
    const Lens perspective;

    CY_CHECK_EQ(blend(perspective, ortho, 0.49F).projection,
                cy::render::ProjectionKind::Perspective);
    CY_CHECK_EQ(blend(perspective, ortho, 0.51F).projection,
                cy::render::ProjectionKind::Orthographic);
}

CY_TEST_CASE("an infinite far plane stays infinite through a blend") {
    // Zero means infinite. Lerping it toward a finite value would walk the far plane out from the
    // near plane rather than in from infinity — everything would be clipped for most of the blend.
    Lens finite;
    finite.gameplay.far_plane = 500.0F;
    const Lens infinite;  // far_plane == 0

    CY_CHECK_EQ(blend(infinite, finite, 0.25F).gameplay.far_plane, 0.0F);
    CY_CHECK_EQ(blend(finite, infinite, 0.75F).gameplay.far_plane, 0.0F);
    CY_CHECK_NEAR(blend(finite, finite, 0.5F).gameplay.far_plane, 500.0F, 1e-3F);
}

CY_TEST_CASE("a lens produces a semantic projection and nothing a backend would recognise") {
    Lens lens = physical(35.0F);
    lens.gameplay.near_plane = 0.25F;
    lens.ortho_height = 12.0F;

    const cy::render::Projection projection = lens.to_projection();
    CY_CHECK_EQ(projection.kind, cy::render::ProjectionKind::Perspective);
    CY_CHECK_NEAR(projection.fov_y_radians, lens.vertical_fov_radians(), 1e-6F);
    CY_CHECK_NEAR(projection.near_plane, 0.25F, 1e-6F);
    CY_CHECK_EQ(projection.far_plane, 0.0F);
    CY_CHECK_NEAR(projection.ortho_height, 12.0F, 1e-6F);
}

CY_TEST_CASE("a degenerate lens is clamped rather than producing a NaN") {
    // An authored asset with a zero in it should produce a usable camera and a diagnostic
    // elsewhere, not an abort and not a matrix full of NaNs.
    CY_CHECK_GT(fov_from_focal_length(0.0F, kSensor), 0.0F);
    CY_CHECK_GT(focal_length_from_fov(0.0F, kSensor), 0.0F);
    CY_CHECK_GT(fov_from_focal_length(35.0F, 0.0F), 0.0F);
}
