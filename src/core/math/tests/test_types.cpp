// The math types: vectors, quaternions, matrices, transforms, shapes and colour. Task 3.1.1.
//
// The conventions themselves are asserted in test_conventions.cpp; this file is about the
// operations being *correct*, which is a different question and a much duller one. It is written
// against the properties that are easy to state and hard to get accidentally right — an inverse
// composed with its original is the identity, a decomposed transform recomposes, a normal matrix
// keeps normals perpendicular — rather than against tables of expected numbers, which only assert
// that the implementation still does what it did the day it was written.

#include <cy/core/math/math.h>

#include <cy/test/test.h>

#include "approx.h"

#include <cmath>
#include <limits>

CY_TEST_CASE("Vec3: the operations agree with their definitions") {
    const cy::Vec3 a{1.0f, 2.0f, 3.0f};
    const cy::Vec3 b{-4.0f, 0.5f, 2.0f};

    CY_CHECK(a + b == cy::Vec3{-3.0f, 2.5f, 5.0f});
    CY_CHECK(a - b == cy::Vec3{5.0f, 1.5f, 1.0f});
    CY_CHECK(a * 2.0f == cy::Vec3{2.0f, 4.0f, 6.0f});
    CY_CHECK_EQ(dot(a, b), (1.0f * -4.0f) + (2.0f * 0.5f) + (3.0f * 2.0f));
    CY_CHECK_EQ(length_squared(cy::Vec3{3.0f, 4.0f, 0.0f}), 25.0f);
    CY_CHECK_EQ(length(cy::Vec3{3.0f, 4.0f, 0.0f}), 5.0f);

    // The cross product is perpendicular to both, which is the property everything downstream
    // relies on and is independent of the sign convention the handedness test pins down.
    const cy::Vec3 perpendicular = cross(a, b);
    CY_CHECK(cy::math::nearly_zero(dot(perpendicular, a), 1e-5f));
    CY_CHECK(cy::math::nearly_zero(dot(perpendicular, b), 1e-5f));

    CY_CHECK_CLOSE(length(normalize(a)), 1.0f, 1e-6f);
    CY_CHECK(normalized_or(cy::Vec3{}, cy::kAxisUp) == cy::kAxisUp);

    // `any_perpendicular` must work for every input direction, including the ones near the axis it
    // uses internally — that branch is exactly where a naive implementation degenerates.
    for (const cy::Vec3 direction :
         {cy::kAxisX, cy::kAxisY, cy::kAxisZ, cy::Vec3{-1.0f, 0.0f, 0.0f},
          normalize(cy::Vec3{0.99f, 0.1f, 0.05f})}) {
        const cy::Vec3 other = any_perpendicular(direction);
        CY_CHECK_CLOSE(length(other), 1.0f, 1e-5f);
        CY_CHECK(cy::math::nearly_zero(dot(other, direction), 1e-5f));
    }
}

CY_TEST_CASE("Vec3: reflect mirrors the component along the normal") {
    const cy::Vec3 incoming = normalize(cy::Vec3{1.0f, -1.0f, 0.0f});
    const cy::Vec3 reflected = reflect(incoming, cy::kAxisUp);
    CY_CHECK(nearly_equal(reflected, normalize(cy::Vec3{1.0f, 1.0f, 0.0f}), 1e-6f));
    // Reflection preserves length and is its own inverse.
    CY_CHECK_CLOSE(length(reflected), length(incoming), 1e-6f);
    CY_CHECK(nearly_equal(reflect(reflected, cy::kAxisUp), incoming, 1e-6f));
}

CY_TEST_CASE("IVec3: floor_to_ivec3 puts negative coordinates in the cell below") {
    // Truncation toward zero would make the cell containing the origin twice as wide as every
    // other one, which is a grid bug that only shows up near the origin.
    CY_CHECK(cy::floor_to_ivec3(cy::Vec3{0.5f, 0.5f, 0.5f}) == cy::IVec3{0, 0, 0});
    CY_CHECK(cy::floor_to_ivec3(cy::Vec3{-0.5f, -0.5f, -0.5f}) == cy::IVec3{-1, -1, -1});
    CY_CHECK(cy::floor_to_ivec3(cy::Vec3{-1.0f, 1.0f, -2.5f}) == cy::IVec3{-1, 1, -3});
}

CY_TEST_CASE("Quat: composition, inversion and rotation are consistent") {
    const cy::Quat a = cy::Quat::from_axis_angle(normalize(cy::Vec3{1.0f, 2.0f, -0.5f}), 0.9f);
    const cy::Quat b = cy::Quat::from_axis_angle(normalize(cy::Vec3{-0.2f, 1.0f, 3.0f}), -2.1f);
    const cy::Vec3 point{0.7f, -1.4f, 2.2f};

    CY_CHECK(nearly_equal((a * b) * point, a * (b * point), 1e-5f));
    CY_CHECK(nearly_equal(inverse(a) * (a * point), point, 1e-5f));
    CY_CHECK(same_rotation(a * inverse(a), cy::Quat::identity(), 1e-6f));

    // A rotation preserves length: that is what distinguishes it from the general linear map a
    // matrix could hold.
    CY_CHECK_CLOSE(length(a * point), length(point), 1e-5f);

    // The double cover: q and -q are the same rotation, and `same_rotation` knows it while
    // `nearly_equal` deliberately does not.
    CY_CHECK(same_rotation(a, -a, 1e-6f));
    CY_CHECK_FALSE(nearly_equal(a, -a, 1e-6f));
}

CY_TEST_CASE("Quat: slerp is constant-angular-velocity and nlerp is not") {
    const cy::Quat start = cy::Quat::identity();
    const cy::Quat end = cy::Quat::from_axis_angle(cy::kAxisY, cy::math::radians(160.0f));

    // Half way along a slerp is half the angle. The same point on an nlerp is not, which is the
    // whole reason both exist and why animation blending uses the more expensive one.
    const cy::Quat halfway = slerp(start, end, 0.5f);
    CY_CHECK_CLOSE(angle_between(start, halfway), cy::math::radians(80.0f), 1e-4f);
    CY_CHECK_CLOSE(angle_between(halfway, end), cy::math::radians(80.0f), 1e-4f);
    CY_CHECK(angle_between(start, nlerp(start, end, 0.5f)) > 0.0f);

    // The endpoints are exact.
    CY_CHECK(same_rotation(slerp(start, end, 0.0f), start, 1e-6f));
    CY_CHECK(same_rotation(slerp(start, end, 1.0f), end, 1e-6f));

    // And slerp takes the short way round even when the two are stored on opposite hemispheres.
    CY_CHECK(same_rotation(slerp(start, -end, 0.5f), halfway, 1e-5f));
}

CY_TEST_CASE("Quat: from_to produces the shortest rotation, including the antiparallel case") {
    const cy::Vec3 from = normalize(cy::Vec3{1.0f, 0.2f, -0.3f});
    const cy::Vec3 to = normalize(cy::Vec3{-0.4f, 1.0f, 0.6f});
    CY_CHECK(nearly_equal(cy::Quat::from_to(from, to) * from, to, 1e-5f));

    // Parallel: nothing to do.
    CY_CHECK(same_rotation(cy::Quat::from_to(from, from), cy::Quat::identity(), 1e-5f));

    // Antiparallel: every axis perpendicular to `from` is equally correct, and a naive
    // implementation produces a zero-length cross product and a NaN.
    const cy::Vec3 flipped = cy::Quat::from_to(from, -from) * from;
    CY_CHECK(is_finite(flipped));
    CY_CHECK(nearly_equal(flipped, -from, 1e-5f));
}

CY_TEST_CASE("Mat4: inverse and inverse_affine undo the original") {
    const cy::Mat4 m =
        cy::Mat4::from_trs(cy::Vec3{3.0f, -1.0f, 7.0f}, cy::Quat::from_axis_angle(cy::kAxisZ, 1.2f),
                           cy::Vec3{2.0f, 0.5f, 1.5f});

    const cy::Expected<cy::Mat4, cy::Error> general = inverse(m);
    CY_REQUIRE(general.has_value());
    CY_CHECK(nearly_equal(*general * m, cy::Mat4::identity(), 1e-4f));

    const cy::Expected<cy::Mat4, cy::Error> affine = inverse_affine(m);
    CY_REQUIRE(affine.has_value());
    CY_CHECK(nearly_equal(*affine * m, cy::Mat4::identity(), 1e-4f));
    // The cheap path and the general one must agree; that they are computed differently is the
    // point of testing both.
    CY_CHECK(nearly_equal(*affine, *general, 1e-4f));

    // A singular matrix is data, not a programmer error: it reports instead of asserting.
    const cy::Mat4 flattened = cy::Mat4::from_scale(cy::Vec3{1.0f, 0.0f, 1.0f});
    const cy::Expected<cy::Mat4, cy::Error> impossible = inverse(flattened);
    CY_CHECK_FALSE(impossible.has_value());
    CY_CHECK(impossible.error().code == cy::ErrorCode::InvalidArgument);
}

CY_TEST_CASE("Mat3: the normal matrix keeps normals perpendicular under non-uniform scale") {
    // The case that motivates the normal matrix at all: scale one axis and a plain transform of the
    // normal is no longer perpendicular to the transformed surface.
    const cy::Mat3 linear = cy::Mat3::from_scale(cy::Vec3{3.0f, 1.0f, 1.0f});
    const cy::Vec3 tangent = normalize(cy::Vec3{1.0f, 1.0f, 0.0f});
    const cy::Vec3 normal = normalize(cy::Vec3{-1.0f, 1.0f, 0.0f});
    CY_CHECK(cy::math::nearly_zero(dot(tangent, normal), 1e-6f));

    const cy::Vec3 moved_tangent = linear * tangent;
    CY_CHECK_FALSE(cy::math::nearly_zero(dot(moved_tangent, linear * normal), 1e-3f));
    CY_CHECK(cy::math::nearly_zero(dot(moved_tangent, cy::normal_matrix(linear) * normal), 1e-5f));
}

CY_TEST_CASE("Mat3: to_quat and from_quat are inverses") {
    const cy::Quat rotation =
        cy::Quat::from_axis_angle(normalize(cy::Vec3{0.3f, -1.0f, 0.7f}), 2.7f);
    CY_CHECK(same_rotation(cy::to_quat(cy::Mat3::from_quat(rotation)), rotation, 1e-5f));

    // Near a half-turn is where the naive single-branch conversion loses all its precision, so it
    // is checked explicitly rather than left to the random case above.
    const cy::Quat half_turn = cy::Quat::from_axis_angle(cy::kAxisX, cy::math::kPi - 1e-3f);
    CY_CHECK(same_rotation(cy::to_quat(cy::Mat3::from_quat(half_turn)), half_turn, 1e-4f));
}

CY_TEST_CASE("Transform: interpolation lerps translation and scale and slerps rotation") {
    // `core-math` — "TRS interpolation": no matrix decomposition and no shear artifacts.
    const cy::Transform a{cy::Quat::identity(), cy::Vec3{0.0f, 0.0f, 0.0f},
                          cy::Vec3{1.0f, 1.0f, 1.0f}};
    const cy::Transform b{cy::Quat::from_axis_angle(cy::kAxisY, cy::math::radians(90.0f)),
                          cy::Vec3{10.0f, 4.0f, -2.0f}, cy::Vec3{3.0f, 3.0f, 3.0f}};

    const cy::Transform middle = interpolate(a, b, 0.5f);
    CY_CHECK(nearly_equal(middle.translation, cy::Vec3{5.0f, 2.0f, -1.0f}, 1e-6f));
    CY_CHECK(nearly_equal(middle.scale, cy::Vec3{2.0f, 2.0f, 2.0f}, 1e-6f));
    CY_CHECK_CLOSE(angle_between(a.rotation, middle.rotation), cy::math::radians(45.0f), 1e-4f);

    // The interpolated rotation stays a rotation: it preserves length. A componentwise matrix lerp
    // would not, and would shear the object half way through.
    CY_CHECK_CLOSE(length(middle.rotation * cy::Vec3{1.0f, 2.0f, 3.0f}),
                   length(cy::Vec3{1.0f, 2.0f, 3.0f}), 1e-5f);
}

CY_TEST_CASE("Transform: inverse is exact for uniform scale") {
    const cy::Transform t{cy::Quat::from_axis_angle(normalize(cy::Vec3{1.0f, 1.0f, 0.0f}), 0.6f),
                          cy::Vec3{4.0f, -2.0f, 1.0f}, cy::Vec3{2.0f, 2.0f, 2.0f}};
    const cy::Vec3 point{1.5f, 0.25f, -3.0f};
    CY_CHECK(nearly_equal(inverse(t).transform_point(t.transform_point(point)), point, 1e-4f));
    CY_CHECK(nearly_equal((t * inverse(t)).translation, cy::Vec3{}, 1e-4f));
}

CY_TEST_CASE("Transform: decompose recovers what from_trs built") {
    const cy::Transform original{cy::Quat::from_axis_angle(cy::kAxisX, -0.9f),
                                 cy::Vec3{-3.0f, 8.0f, 0.5f}, cy::Vec3{1.5f, 2.0f, 0.75f}};
    const cy::Expected<cy::Transform, cy::Error> recovered = decompose(original.to_matrix());
    CY_REQUIRE(recovered.has_value());
    CY_CHECK(nearly_equal(*recovered, original, 1e-4f));

    // A matrix that is not affine has no TRS, and that is reported rather than asserted: it can
    // arrive from an importer.
    cy::Mat4 projective = cy::Mat4::identity();
    projective.at(3, 2) = -1.0f;
    CY_CHECK_FALSE(decompose(projective).has_value());
}

CY_TEST_CASE("Transform2D: composition and inversion behave like their 3D counterparts") {
    const cy::Transform2D a{cy::Vec2{10.0f, 5.0f}, cy::Vec2{1.0f, 1.0f}, cy::math::radians(30.0f)};
    const cy::Transform2D b{cy::Vec2{-2.0f, 3.0f}, cy::Vec2{1.0f, 1.0f}, cy::math::radians(-80.0f)};
    const cy::Vec2 point{4.0f, -1.0f};

    CY_CHECK(nearly_equal((a * b).transform_point(point),
                          a.transform_point(b.transform_point(point)), 1e-4f));
    CY_CHECK(nearly_equal(inverse(a).transform_point(a.transform_point(point)), point, 1e-4f));

    // The 3x3 form agrees with the direct evaluation, which is what a UI batch uploads.
    const cy::Vec3 through_matrix = a.to_matrix() * cy::Vec3{point.x, point.y, 1.0f};
    CY_CHECK(nearly_equal(cy::Vec2{through_matrix.x, through_matrix.y}, a.transform_point(point),
                          1e-4f));
}

CY_TEST_CASE("Aabb: the empty box is the identity for growing") {
    cy::Aabb box = cy::Aabb::empty();
    CY_CHECK(box.is_empty());
    CY_CHECK_EQ(box.surface_area(), 0.0f);

    // Accumulating over zero points leaves it empty rather than leaving a degenerate box at the
    // origin, which would swallow the centre of the world into every bound built this way.
    box.grow(cy::Vec3{1.0f, 2.0f, 3.0f});
    CY_CHECK_FALSE(box.is_empty());
    CY_CHECK(box.min == cy::Vec3{1.0f, 2.0f, 3.0f});
    CY_CHECK(box.max == cy::Vec3{1.0f, 2.0f, 3.0f});

    box.grow(cy::Vec3{-1.0f, 5.0f, 0.0f});
    CY_CHECK(box.min == cy::Vec3{-1.0f, 2.0f, 0.0f});
    CY_CHECK(box.max == cy::Vec3{1.0f, 5.0f, 3.0f});
    CY_CHECK_EQ(box.surface_area(), 2.0f * ((2.0f * 3.0f) + (3.0f * 3.0f) + (3.0f * 2.0f)));
}

CY_TEST_CASE("Aabb: corner indexing matches the frustum's sign-mask bit order") {
    const cy::Aabb box{cy::Vec3{-1.0f, -2.0f, -3.0f}, cy::Vec3{4.0f, 5.0f, 6.0f}};
    CY_CHECK(box.corner(0) == box.min);
    CY_CHECK(box.corner(7) == box.max);
    CY_CHECK(box.corner(1) == cy::Vec3{4.0f, -2.0f, -3.0f});
    CY_CHECK(box.corner(2) == cy::Vec3{-1.0f, 5.0f, -3.0f});
    CY_CHECK(box.corner(4) == cy::Vec3{-1.0f, -2.0f, 6.0f});

    CY_CHECK(box.closest_point(cy::Vec3{100.0f, 0.0f, -100.0f}) == cy::Vec3{4.0f, 0.0f, -3.0f});
    CY_CHECK(box.closest_point(cy::Vec3{0.0f, 0.0f, 0.0f}) == cy::Vec3{0.0f, 0.0f, 0.0f});
}

CY_TEST_CASE("Aabb: a transformed box contains the transformed corners") {
    const cy::Aabb box{cy::Vec3{-1.0f, -1.0f, -1.0f}, cy::Vec3{1.0f, 1.0f, 1.0f}};
    const cy::Mat4 m =
        cy::Mat4::from_trs(cy::Vec3{5.0f, 0.0f, -2.0f}, cy::Quat::from_axis_angle(cy::kAxisY, 0.7f),
                           cy::Vec3{2.0f, 1.0f, 1.0f});
    const cy::Aabb moved = transformed(box, m);

    // Conservative in the stated direction: it contains every transformed corner, and may be a
    // little larger than the tightest box that does.
    for (cy::u32 i = 0; i < 8; ++i) {
        CY_CHECK(moved.contains(cy::transform_point(m, box.corner(i))));
    }
    CY_CHECK(transformed(cy::Aabb::empty(), m).is_empty());
}

CY_TEST_CASE("Obb: bounds contain it and containment is exact in its own frame") {
    const cy::Obb obb{cy::Vec3{1.0f, 2.0f, 3.0f}, cy::Vec3{2.0f, 0.5f, 1.0f},
                      cy::Quat::from_axis_angle(cy::kAxisY, cy::math::radians(45.0f))};
    // Every corner sits exactly on both boundaries, and a round trip through the rotation moves it
    // by a few ULPs — so both tests are made against a hair of slack rather than against an exact
    // boundary, which would be asserting the rounding rather than the geometry.
    const cy::Aabb bounds = obb.bounds();
    for (cy::u32 i = 0; i < 8; ++i) {
        const cy::Vec3 corner = obb.corner(i);
        CY_CHECK(bounds.expanded(1e-4f).contains(corner));
        CY_CHECK(obb.contains(obb.center + (corner - obb.center) * 0.999f));
    }
    CY_CHECK(obb.contains(obb.center));
    CY_CHECK_FALSE(obb.contains(obb.center + cy::Vec3{0.0f, 1.0f, 0.0f}));
}

CY_TEST_CASE("Plane: the signed distance is positive on the normal's side") {
    const cy::Plane plane = cy::Plane::from_point_normal(cy::Vec3{0.0f, 2.0f, 0.0f}, cy::kAxisUp);
    CY_CHECK_EQ(plane.signed_distance(cy::Vec3{0.0f, 5.0f, 0.0f}), 3.0f);
    CY_CHECK_EQ(plane.signed_distance(cy::Vec3{0.0f, 2.0f, 0.0f}), 0.0f);
    CY_CHECK_EQ(plane.signed_distance(cy::Vec3{0.0f, 0.0f, 0.0f}), -2.0f);
    CY_CHECK(plane.project(cy::Vec3{7.0f, 9.0f, -1.0f}) == cy::Vec3{7.0f, 2.0f, -1.0f});

    // The winding convention: counter-clockwise around a → b → c faces the normal.
    const cy::Plane from_points = cy::Plane::from_points(
        cy::Vec3{0.0f, 0.0f, 0.0f}, cy::Vec3{1.0f, 0.0f, 0.0f}, cy::Vec3{0.0f, 0.0f, -1.0f});
    CY_CHECK(nearly_equal(from_points.normal, cy::kAxisUp, 1e-6f));
}

CY_TEST_CASE("Frustum: the AABB test is conservative and never omits an intersecting box") {
    const cy::Mat4 view = cy::look_at(cy::Vec3{0.0f, 0.0f, 5.0f}, cy::Vec3{0.0f, 0.0f, 0.0f});
    const cy::Mat4 projection =
        cy::perspective_reversed_z(cy::math::radians(60.0f), 1.0f, 0.5f, 50.0f);
    const cy::Frustum frustum = cy::Frustum::from_view_projection(projection * view);

    // A box around the look-at target is certainly visible.
    CY_CHECK(
        frustum.intersects(cy::Aabb::from_center_extents(cy::Vec3{}, cy::Vec3{1.0f, 1.0f, 1.0f})));
    // One far off to the side is certainly not.
    CY_CHECK_FALSE(frustum.intersects(
        cy::Aabb::from_center_extents(cy::Vec3{500.0f, 0.0f, 0.0f}, cy::Vec3{1.0f, 1.0f, 1.0f})));
    // One behind the camera is certainly not.
    CY_CHECK_FALSE(frustum.intersects(
        cy::Aabb::from_center_extents(cy::Vec3{0.0f, 0.0f, 100.0f}, cy::Vec3{1.0f, 1.0f, 1.0f})));
    CY_CHECK_FALSE(frustum.intersects(cy::Aabb::empty()));

    // The direction that matters: a box containing a point the frustum contains must be accepted.
    // A false negative is a hole in the image; a false positive only costs a draw call.
    CY_CHECK(frustum.intersects(cy::Sphere{cy::Vec3{}, 1.0f}));
    CY_CHECK_FALSE(frustum.intersects(cy::Sphere{cy::Vec3{0.0f, 0.0f, 200.0f}, 1.0f}));
}

CY_TEST_CASE("Color: the sRGB boundary round-trips and alpha stays linear") {
    // The two anchors of the transfer function.
    CY_CHECK_EQ(cy::srgb_to_linear(0.0f), 0.0f);
    CY_CHECK_CLOSE(cy::srgb_to_linear(1.0f), 1.0f, 1e-6f);
    // Mid grey, the number every colour pipeline is checked against: 0.5 encoded is 0.2140 linear.
    CY_CHECK_CLOSE(cy::srgb_to_linear(0.5f), 0.21404f, 1e-3f);
    CY_CHECK_CLOSE(cy::linear_to_srgb(cy::srgb_to_linear(0.73f)), 0.73f, 1e-5f);

    const cy::Color color = cy::Color::from_srgb8(128, 64, 255, 128);
    // Alpha is *not* gamma-encoded: 128/255 comes through unchanged.
    CY_CHECK_CLOSE(color.a, 128.0f / 255.0f, 1e-6f);
    CY_CHECK(color.r < 128.0f / 255.0f);  // decoding darkens the mid-tones

    cy::u8 r = 0;
    cy::u8 g = 0;
    cy::u8 b = 0;
    cy::u8 a = 0;
    color.to_srgb8(r, g, b, a);
    CY_CHECK_EQ(static_cast<int>(r), 128);
    CY_CHECK_EQ(static_cast<int>(g), 64);
    CY_CHECK_EQ(static_cast<int>(b), 255);
    CY_CHECK_EQ(static_cast<int>(a), 128);

    CY_CHECK(cy::Color::from_srgb_hex(0x8040FF80u) == color);
    CY_CHECK_CLOSE(cy::colors::kWhite.luminance(), 1.0f, 1e-6f);
}

// `clamp` and `sign` were nested conditional operators until the lint gate asked for guards. The
// rewrite is only equivalent if the fallthrough order survives, and the cases where that is
// observable are the ones nobody writes down: what a NaN does, and which of the two signed zeroes
// comes back. This case is the regression test for that rewrite, not a test of the arithmetic.
CY_TEST_CASE("clamp and sign: the edge cases the guard form has to preserve") {
    const cy::f32 nan = std::numeric_limits<cy::f32>::quiet_NaN();

    // Ordinary behaviour first, so a failure below is read as an edge case and not as a typo.
    CY_CHECK_EQ(cy::math::clamp(0.5f, 0.0f, 1.0f), 0.5f);
    CY_CHECK_EQ(cy::math::clamp(-1.0f, 0.0f, 1.0f), 0.0f);
    CY_CHECK_EQ(cy::math::clamp(2.0f, 0.0f, 1.0f), 1.0f);
    // Exactly on a bound returns the value, not the bound: neither guard fires.
    CY_CHECK_EQ(cy::math::clamp(0.0f, 0.0f, 1.0f), 0.0f);
    CY_CHECK_EQ(cy::math::clamp(1.0f, 0.0f, 1.0f), 1.0f);
    // An inverted range: the low guard is tested first, so `low` wins.
    CY_CHECK_EQ(cy::math::clamp(0.5f, 1.0f, 0.0f), 1.0f);

    // A NaN compares false against both bounds and falls through unchanged. `saturate` inherits it.
    CY_CHECK(std::isnan(cy::math::clamp(nan, 0.0f, 1.0f)));
    CY_CHECK(std::isnan(cy::math::saturate(nan)));

    CY_CHECK_EQ(cy::math::sign(3.0f), 1.0f);
    CY_CHECK_EQ(cy::math::sign(-3.0f), -1.0f);
    // Both signed zeroes and a NaN reach the final return, which is 0.0f and not the input.
    CY_CHECK_EQ(cy::math::sign(0.0f), 0.0f);
    CY_CHECK_EQ(cy::math::sign(-0.0f), 0.0f);
    CY_CHECK_EQ(cy::math::sign(nan), 0.0f);
    CY_CHECK(!std::isnan(cy::math::sign(nan)));

    // Both stay usable in a constant expression, which the conditional form also was: the guards
    // are not allowed to have cost that.
    static_assert(cy::math::clamp(2.0f, 0.0f, 1.0f) == 1.0f);
    static_assert(cy::math::sign(-3.0f) == -1.0f);
}
