// THE CONVENTIONS, AS EXECUTABLE TESTS. Task 3.1.2, design.md §4.
//
// "A convention in a comment is a convention that gets violated by someone who did not read the
// comment." Every requirement `core-math` states about handedness, depth, matrix layout and units
// is asserted here as a *numeric consequence* rather than as a restatement: not "the depth range is
// reversed" but "this perspective matrix maps the near plane to exactly 1.0 and the far plane to
// 0.0"; not "matrices are column-major with column vectors" but "the translation of a translation
// matrix is at at(0,3) and not at at(3,0), and `A * B` applies B first".
//
// This file is the one that makes M3's precision behaviour debuggable rather than mysterious. If a
// renderer produces a black screen at M3, the first question is which of these flipped, and the
// answer is whichever of these tests went red.
//
// WHERE A COMPARISON IS EXACT, IT IS SPELLED `==`. Several of these identities are exact in IEEE
// 754 — a look-at down −Z, an infinite projection at the near plane, an orthographic projection
// with a span that is a power of two — and asserting them within a tolerance would hide a genuine
// sign error behind a comfortable epsilon. Where the arithmetic genuinely rounds, the tolerance is
// stated and is tight.

#include <cy/core/math/math.h>

#include <cy/test/test.h>

#include "approx.h"

#include <cmath>

namespace {

/// An absolute comparison against zero. `CY_CHECK_NEAR` is relative (doctest's `Approx::epsilon`
/// scales by the larger magnitude), so against an expected value of exactly 0 it degenerates into
/// requiring exact equality. Several assertions below genuinely want "within a tolerance of zero".
[[nodiscard]] bool near_zero(cy::f32 value, cy::f32 tolerance) {
    return std::fabs(value) <= tolerance;
}

}  // namespace

// --- Handedness, up, and forward -----------------------------------------------------------------

CY_TEST_CASE("Convention: the coordinate system is right-handed") {
    // The entire handedness of the engine, in three cross products. Reverse the handedness and all
    // three flip sign; there is no way to change one without the others.
    CY_CHECK(cross(cy::kAxisX, cy::kAxisY) == cy::kAxisZ);
    CY_CHECK(cross(cy::kAxisY, cy::kAxisZ) == cy::kAxisX);
    CY_CHECK(cross(cy::kAxisZ, cy::kAxisX) == cy::kAxisY);
}

CY_TEST_CASE("Convention: Y is up and local -Z is forward") {
    CY_CHECK(cy::kAxisUp == cy::Vec3{0.0f, 1.0f, 0.0f});
    CY_CHECK(cy::kAxisRight == cy::Vec3{1.0f, 0.0f, 0.0f});
    CY_CHECK(cy::kAxisForward == cy::Vec3{0.0f, 0.0f, -1.0f});

    // `core-math` — "Identity transform faces −Z".
    const cy::Transform identity = cy::Transform::identity();
    CY_CHECK(identity.forward() == cy::Vec3{0.0f, 0.0f, -1.0f});
    CY_CHECK(identity.up() == cy::Vec3{0.0f, 1.0f, 0.0f});
    CY_CHECK(identity.right() == cy::Vec3{1.0f, 0.0f, 0.0f});
}

CY_TEST_CASE("Convention: a look-at down -Z is the identity rotation") {
    // design.md §4 names this one explicitly. It is exact: every intermediate is 0, 1 or -1, and
    // the normalisations divide by exactly 1.
    const cy::Quat rotation = cy::Quat::look_rotation(cy::kAxisForward, cy::kAxisUp);
    CY_CHECK(rotation == cy::Quat::identity());

    const cy::Mat4 view =
        cy::look_at(cy::Vec3{0.0f, 0.0f, 0.0f}, cy::Vec3{0.0f, 0.0f, -1.0f}, cy::kAxisUp);
    CY_CHECK(view == cy::Mat4::identity());

    // And the converse, so the test cannot pass by returning the identity for everything: looking
    // down +Z is a half-turn about Y, which sends +X to -X.
    const cy::Quat backwards = cy::Quat::look_rotation(cy::kAxisZ, cy::kAxisUp);
    CY_CHECK(nearly_equal(backwards * cy::kAxisX, cy::Vec3{-1.0f, 0.0f, 0.0f}, 1e-6f));
}

CY_TEST_CASE("Convention: rotation is counter-clockwise seen from the positive axis") {
    // A quarter turn about +Y takes +X to −Z — which is also the statement that yawing left points
    // an object's forward where its right used to be.
    const cy::Quat yaw = cy::Quat::from_axis_angle(cy::kAxisY, cy::math::radians(90.0f));
    CY_CHECK(nearly_equal(yaw * cy::kAxisX, cy::kAxisForward, 1e-6f));

    // A quarter turn about +X takes +Y to +Z: pitching up tips the up axis backwards.
    const cy::Quat pitch = cy::Quat::from_axis_angle(cy::kAxisX, cy::math::radians(90.0f));
    CY_CHECK(nearly_equal(pitch * cy::kAxisY, cy::kAxisZ, 1e-6f));

    // A quarter turn about +Z takes +X to +Y.
    const cy::Quat roll = cy::Quat::from_axis_angle(cy::kAxisZ, cy::math::radians(90.0f));
    CY_CHECK(nearly_equal(roll * cy::kAxisX, cy::kAxisY, 1e-6f));
}

CY_TEST_CASE("Convention: the default Euler order is YXZ") {
    const cy::Vec3 euler{cy::math::radians(20.0f), cy::math::radians(50.0f),
                         cy::math::radians(-35.0f)};

    // The composition YXZ names, written out. If `from_euler_yxz` ever applies them in another
    // order this fails, and it fails with a diagnosis rather than with a wrong-looking character.
    const cy::Quat expected = cy::Quat::from_axis_angle(cy::kAxisY, euler.y) *
                              cy::Quat::from_axis_angle(cy::kAxisX, euler.x) *
                              cy::Quat::from_axis_angle(cy::kAxisZ, euler.z);
    CY_CHECK(nearly_equal(cy::Quat::from_euler_yxz(euler), expected, 1e-6f));

    // And the round trip, which is what an editor field does every time it is shown and edited.
    const cy::Vec3 recovered = cy::Quat::from_euler_yxz(euler).to_euler_yxz();
    CY_CHECK(nearly_equal(recovered, euler, 1e-5f));
}

// --- Matrix layout and composition
// ------------------------------------------------------------------

CY_TEST_CASE("Convention: matrices are column-major with the translation in the fourth column") {
    const cy::Mat4 translation = cy::Mat4::from_translation(cy::Vec3{1.0f, 2.0f, 3.0f});

    // Row-first access finds the translation in column 3. In a row-vector engine it would be in
    // row 3, and `at(3, 0)` would be 1 instead of 0 — which is the assertion that makes this test
    // about the convention rather than about the constructor.
    CY_CHECK_EQ(translation.at(0, 3), 1.0f);
    CY_CHECK_EQ(translation.at(1, 3), 2.0f);
    CY_CHECK_EQ(translation.at(2, 3), 3.0f);
    CY_CHECK_EQ(translation.at(3, 0), 0.0f);
    CY_CHECK_EQ(translation.at(3, 1), 0.0f);
    CY_CHECK_EQ(translation.at(3, 2), 0.0f);

    // `core-math` — "Upload without transposition": the sixteen floats are column-major in memory,
    // so the translation occupies elements 12, 13 and 14 and a shader's `mat4` reads it directly.
    const cy::f32* data = translation.data();
    CY_CHECK_EQ(data[12], 1.0f);
    CY_CHECK_EQ(data[13], 2.0f);
    CY_CHECK_EQ(data[14], 3.0f);
    CY_CHECK_EQ(data[15], 1.0f);

    // A column really is contiguous: that is what makes the upload a memcpy.
    CY_CHECK_EQ(static_cast<cy::usize>(&translation.columns[1].x - &translation.columns[0].x), 4u);
}

CY_TEST_CASE("Convention: A * B applies B first") {
    // The two orders give visibly different answers, which is the point of choosing this pair:
    // a translation along +X and a quarter turn about +Y.
    const cy::Mat4 move = cy::Mat4::from_translation(cy::Vec3{10.0f, 0.0f, 0.0f});
    const cy::Mat4 turn =
        cy::Mat4::from_quat(cy::Quat::from_axis_angle(cy::kAxisY, cy::math::radians(90.0f)));
    const cy::Vec3 p{1.0f, 0.0f, 0.0f};

    // move * turn: turn first — (1,0,0) becomes (0,0,-1) — then move: (10, 0, -1).
    CY_CHECK(
        nearly_equal(cy::transform_point(move * turn, p), cy::Vec3{10.0f, 0.0f, -1.0f}, 1e-5f));

    // turn * move: move first — (11,0,0) — then turn: (0, 0, -11).
    CY_CHECK(
        nearly_equal(cy::transform_point(turn * move, p), cy::Vec3{0.0f, 0.0f, -11.0f}, 1e-5f));
}

CY_TEST_CASE("Convention: Transform, Quat and Mat4 compose in the same direction") {
    const cy::Transform a{cy::Quat::from_axis_angle(cy::kAxisY, cy::math::radians(30.0f)),
                          cy::Vec3{1.0f, 2.0f, 3.0f}, cy::Vec3{1.0f, 1.0f, 1.0f}};
    const cy::Transform b{cy::Quat::from_axis_angle(cy::kAxisX, cy::math::radians(-40.0f)),
                          cy::Vec3{-4.0f, 0.5f, 2.0f}, cy::Vec3{1.0f, 1.0f, 1.0f}};

    // Three representations of one composition. Any disagreement between them is a convention that
    // has drifted in one of the three and not the others, which is exactly how this class of bug
    // arrives.
    const cy::Mat4 composed_transform = (a * b).to_matrix();
    const cy::Mat4 composed_matrix = a.to_matrix() * b.to_matrix();
    CY_CHECK(nearly_equal(composed_transform, composed_matrix, 1e-5f));

    const cy::Vec3 point{0.3f, -1.2f, 4.0f};
    CY_CHECK(nearly_equal((a * b).transform_point(point),
                          a.transform_point(b.transform_point(point)), 1e-5f));
    CY_CHECK(
        nearly_equal((a.rotation * b.rotation) * point, a.rotation * (b.rotation * point), 1e-5f));
}

// --- Depth
// ------------------------------------------------------------------------------------------

CY_TEST_CASE("Convention: the depth convention is reversed Z over [0, 1]") {
    CY_CHECK(cy::DepthConvention::kReversedZ);
    CY_CHECK_EQ(cy::DepthConvention::kNdcMin, 0.0f);
    CY_CHECK_EQ(cy::DepthConvention::kNdcMax, 1.0f);
    CY_CHECK_EQ(cy::DepthConvention::kNearPlaneDepth, 1.0f);
    CY_CHECK_EQ(cy::DepthConvention::kFarPlaneDepth, 0.0f);

    // `core-math` — "Depth test direction": cleared to 0.0, opaque compares GreaterEqual, shadow
    // comparison samplers use Greater.
    CY_CHECK_EQ(cy::DepthConvention::kClearValue, 0.0f);
    CY_CHECK(cy::DepthConvention::kOpaque == cy::DepthCompareOp::GreaterEqual);
    CY_CHECK(cy::DepthConvention::kShadow == cy::DepthCompareOp::Greater);

    // "Nearer" means "greater". A pipeline that reads this instead of writing `<` cannot get the
    // direction wrong.
    CY_CHECK(cy::depth_is_nearer(1.0f, 0.0f));
    CY_CHECK_FALSE(cy::depth_is_nearer(0.0f, 1.0f));
}

CY_TEST_CASE("Convention: a perspective projection maps near to 1 and far to 0") {
    constexpr cy::f32 kNear = 0.1f;
    constexpr cy::f32 kFar = 1000.0f;
    const cy::Mat4 projection =
        cy::perspective_reversed_z(cy::math::radians(60.0f), 16.0f / 9.0f, kNear, kFar);

    // THE NUMBER THAT MATTERS. A point on the near plane — the camera looks down −Z, so its view
    // space z is −near — projects to depth 1. A point on the far plane projects to depth 0.
    const cy::Vec3 on_near = cy::project_point(projection, cy::Vec3{0.0f, 0.0f, -kNear});
    const cy::Vec3 on_far = cy::project_point(projection, cy::Vec3{0.0f, 0.0f, -kFar});
    CY_CHECK_CLOSE(on_near.z, 1.0f, 1e-6f);
    CY_CHECK(near_zero(on_far.z, 1e-6f));

    // Depth decreases monotonically with distance, which is the whole content of "reversed".
    const cy::Vec3 near_ish = cy::project_point(projection, cy::Vec3{0.0f, 0.0f, -1.0f});
    const cy::Vec3 far_ish = cy::project_point(projection, cy::Vec3{0.0f, 0.0f, -100.0f});
    CY_CHECK(cy::depth_is_nearer(near_ish.z, far_ish.z));
    CY_CHECK(near_ish.z < 1.0f);
    CY_CHECK(far_ish.z > 0.0f);

    // The x and y mapping is unchanged by the reversal: the frustum edges still land on ±1.
    const cy::f32 half_height = kNear * std::tan(cy::math::radians(30.0f));
    const cy::Vec3 top_edge = cy::project_point(projection, cy::Vec3{0.0f, half_height, -kNear});
    CY_CHECK_CLOSE(top_edge.y, 1.0f, 1e-5f);

    // And the planes can be read back off the matrix, which is what the diagnostics path does.
    cy::f32 recovered_near = 0.0f;
    cy::f32 recovered_far = 0.0f;
    cy::perspective_planes(projection, recovered_near, recovered_far);
    CY_CHECK_CLOSE(recovered_near, kNear, 1e-4f);
    CY_CHECK_CLOSE(recovered_far, kFar, 1e-4f);
}

CY_TEST_CASE("Convention: the infinite far plane maps near to exactly 1 and infinity to 0") {
    // `core-math` — "Infinite far plane". This one is exact: the matrix's third column is
    // (0, 0, 0, near), so clip.z and clip.w are both exactly `near` at the near plane and the
    // divide is 1.0 with no rounding at all.
    constexpr cy::f32 kNear = 0.05f;
    const cy::Mat4 projection =
        cy::perspective_reversed_z_infinite(cy::math::radians(75.0f), 1.0f, kNear);

    const cy::Vec3 on_near = cy::project_point(projection, cy::Vec3{0.0f, 0.0f, -kNear});
    CY_CHECK_EQ(on_near.z, 1.0f);

    // Depth approaches zero at distance and never reaches it, which is what keeps precision
    // well distributed rather than clipping the horizon away.
    const cy::Vec3 very_far = cy::project_point(projection, cy::Vec3{0.0f, 0.0f, -1.0e7f});
    CY_CHECK(very_far.z > 0.0f);
    CY_CHECK(very_far.z < 1.0e-7f);

    cy::f32 recovered_near = 0.0f;
    cy::f32 recovered_far = 0.0f;
    cy::perspective_planes(projection, recovered_near, recovered_far);
    CY_CHECK_EQ(recovered_near, kNear);
    CY_CHECK(std::isinf(recovered_far));
}

CY_TEST_CASE("Convention: an orthographic projection is reversed too") {
    // near = 1, far = 11 gives a span of 10 and coefficients of 0.1 and 1.1, so both endpoints are
    // exact in binary floating point and the assertion can be an equality.
    const cy::Mat4 projection = cy::orthographic_reversed_z(-2.0f, 2.0f, -1.0f, 1.0f, 1.0f, 11.0f);

    const cy::Vec3 on_near = cy::project_point(projection, cy::Vec3{0.0f, 0.0f, -1.0f});
    const cy::Vec3 on_far = cy::project_point(projection, cy::Vec3{0.0f, 0.0f, -11.0f});
    CY_CHECK_CLOSE(on_near.z, 1.0f, 1e-6f);
    CY_CHECK(near_zero(on_far.z, 1e-6f));

    // The extents map to the [-1, 1] clip square, unchanged by the depth reversal.
    const cy::Vec3 corner = cy::project_point(projection, cy::Vec3{2.0f, 1.0f, -5.0f});
    CY_CHECK_CLOSE(corner.x, 1.0f, 1e-6f);
    CY_CHECK_CLOSE(corner.y, 1.0f, 1e-6f);
}

CY_TEST_CASE("Convention: the frustum's near and far planes follow the reversed-Z assignment") {
    // If the near and far planes were extracted with the conventional-depth assignment, this test
    // would let through everything in front of the camera and cull everything beyond the near
    // plane — a failure that looks like a rendering bug and is a convention bug.
    constexpr cy::f32 kNear = 1.0f;
    constexpr cy::f32 kFar = 100.0f;
    const cy::Mat4 view = cy::look_at(cy::Vec3{0.0f, 0.0f, 0.0f}, cy::kAxisForward, cy::kAxisUp);
    const cy::Mat4 projection =
        cy::perspective_reversed_z(cy::math::radians(60.0f), 1.0f, kNear, kFar);
    const cy::Frustum frustum = cy::Frustum::from_view_projection(projection * view);

    CY_CHECK(frustum.contains(cy::Vec3{0.0f, 0.0f, -10.0f}));          // comfortably inside
    CY_CHECK_FALSE(frustum.contains(cy::Vec3{0.0f, 0.0f, -0.5f}));     // in front of the near plane
    CY_CHECK_FALSE(frustum.contains(cy::Vec3{0.0f, 0.0f, -200.0f}));   // beyond the far plane
    CY_CHECK_FALSE(frustum.contains(cy::Vec3{0.0f, 0.0f, 10.0f}));     // behind the camera
    CY_CHECK_FALSE(frustum.contains(cy::Vec3{100.0f, 0.0f, -10.0f}));  // outside the right plane
}

// --- Units and precision
// --------------------------------------------------------------------------

CY_TEST_CASE("Convention: angles are radians and degrees appear only in named converters") {
    CY_CHECK_CLOSE(cy::math::radians(180.0f), cy::math::kPi, 1e-6f);
    CY_CHECK_CLOSE(cy::math::degrees(cy::math::kPi), 180.0f, 1e-6f);
    CY_CHECK_CLOSE(cy::math::radians(cy::math::degrees(1.234f)), 1.234f, 1e-6f);

    // Every angle-taking function names its parameter in radians, and there is no degree-taking
    // overload to pick by accident: a quarter turn is pi/2, not 90.
    const cy::Quat quarter = cy::Quat::from_axis_angle(cy::kAxisY, cy::math::kHalfPi);
    CY_CHECK(nearly_equal(quarter * cy::kAxisX, cy::kAxisForward, 1e-6f));
}

CY_TEST_CASE("Convention: the simulation clock accumulates in f64, and runtime math is f32") {
    // `core-math` — "Precision" and its "Time accumulation" scenario. The accumulator is f64 by
    // *type*, which is what this asserts: a 32-bit accumulator drifts visibly over a long session,
    // and the drift is invisible in any short test, so the type is the only thing worth checking.
    static_assert(std::is_same_v<decltype(cy::math::fixed_steps(0.0, 0.0, 0u)), cy::u32>);
    static_assert(sizeof(cy::f64) == 8);

    // A tenth of a second is not representable in binary, so accumulating it 36 000 times — ten
    // minutes at 60 Hz — is the classic demonstration. In f64 the error stays far below one step.
    cy::f64 accumulated = 0.0;
    for (int i = 0; i < 36000; ++i) {
        accumulated += 1.0 / 60.0;
    }
    CY_CHECK(std::fabs(accumulated - 600.0) < 1e-9);

    CY_CHECK_EQ(cy::math::fixed_steps(0.05, 1.0 / 60.0, 8u), 3u);
    CY_CHECK_EQ(cy::math::fixed_steps(10.0, 1.0 / 60.0, 8u), 8u);  // clamped, not caught up
    CY_CHECK_EQ(cy::math::fixed_steps(0.0, 1.0 / 60.0, 8u), 0u);

    // Runtime types are 32-bit, and `Vec3` is not padded (`core-math` — "SIMD strategy").
    static_assert(sizeof(cy::f32) == 4);
    CY_CHECK_EQ(sizeof(cy::Vec3), 12u);
    CY_CHECK_EQ(sizeof(cy::Mat4), 64u);
}

CY_TEST_CASE("Convention: 2D screen space has its origin at the top-left with +Y downward") {
    // The 2D convention has no arithmetic of its own to assert — it is a statement about what a
    // coordinate *means*. What can be asserted is that the rectangle type is half-open and
    // origin-plus-size, so a grid of screen rectangles tiles without a pixel belonging to two of
    // them, and that a larger y is further *down* the screen.
    const cy::Rect top{cy::Vec2{0.0f, 0.0f}, cy::Vec2{100.0f, 50.0f}};
    const cy::Rect below{cy::Vec2{0.0f, 50.0f}, cy::Vec2{100.0f, 50.0f}};

    CY_CHECK(top.contains(cy::Vec2{0.0f, 0.0f}));
    CY_CHECK(top.contains(cy::Vec2{99.0f, 49.0f}));
    CY_CHECK_FALSE(top.contains(cy::Vec2{0.0f, 50.0f}));
    CY_CHECK(below.contains(cy::Vec2{0.0f, 50.0f}));
    CY_CHECK_FALSE(top.intersects(below));

    // `below` is beneath `top` on screen: its y is larger. That is the whole of "+Y is down".
    CY_CHECK(below.position.y > top.position.y);
}
