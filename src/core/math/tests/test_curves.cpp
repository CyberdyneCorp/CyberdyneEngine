// Curves, gradients and the easing table. Task 3.1.5.
//
// `core-math` — "Curves and easing", whose one scenario is the load-bearing part:
// **"Constant-speed path following" — sampling by distance uses the baked arc-length table, not
// per-frame integration.** The test for it measures the *speed* along a path whose control points
// are deliberately unevenly spaced, which is exactly the case where a naive parameter sweep speeds
// up and slows down for reasons nothing in the scene explains.

#include <cy/core/math/math.h>

#include <cy/test/test.h>

#include "approx.h"

#include <cmath>
#include <string_view>
#include <vector>

CY_TEST_CASE("Curve: keys interpolate by cubic Hermite and pass through their values") {
    cy::Curve curve;
    curve.add_key(cy::CurveKey{0.0f, 0.0f, 0.0f, 0.0f});
    curve.add_key(cy::CurveKey{1.0f, 10.0f, 0.0f, 0.0f});

    // A curve passes through every key exactly, whatever the tangents.
    CY_CHECK_EQ(curve.evaluate(0.0f), 0.0f);
    CY_CHECK_EQ(curve.evaluate(1.0f), 10.0f);
    // Flat tangents give the smoothstep shape: half way in time is half way in value, and the
    // derivative at both ends is zero.
    CY_CHECK_CLOSE(curve.evaluate(0.5f), 5.0f, 1e-5f);
    CY_CHECK(cy::math::nearly_zero(curve.derivative(0.0f), 1e-5f));
    CY_CHECK(cy::math::nearly_zero(curve.derivative(1.0f), 1e-5f));
    CY_CHECK(curve.derivative(0.5f) > 0.0f);

    // Keys are kept in time order however they arrive.
    cy::Curve unordered;
    unordered.add_key(cy::CurveKey{2.0f, 20.0f, 0.0f, 0.0f});
    unordered.add_key(cy::CurveKey{0.0f, 0.0f, 0.0f, 0.0f});
    unordered.add_key(cy::CurveKey{1.0f, 10.0f, 0.0f, 0.0f});
    CY_CHECK_EQ(unordered.key_count(), 3u);
    CY_CHECK_EQ(unordered.key(0).time, 0.0f);
    CY_CHECK_EQ(unordered.key(2).time, 2.0f);
    CY_CHECK_EQ(unordered.start_time(), 0.0f);
    CY_CHECK_EQ(unordered.end_time(), 2.0f);

    // An empty curve is zero rather than undefined, and a one-key curve is constant.
    const cy::Curve empty;
    CY_CHECK_EQ(empty.evaluate(3.0f), 0.0f);
}

CY_TEST_CASE("Curve: linear tangents produce a straight line between keys") {
    // Tangents of 1 unit per unit of time on both sides of both keys give exactly the straight
    // line, which is the case where the Hermite basis has an answer that can be checked by hand.
    cy::Curve curve;
    curve.add_key(cy::CurveKey{0.0f, 0.0f, 1.0f, 1.0f});
    curve.add_key(cy::CurveKey{2.0f, 2.0f, 1.0f, 1.0f});
    // Stepped by an integer and multiplied, rather than accumulated in f32: the accumulating form
    // reaches the loop bound by luck, and whether the last sample is taken at all depends on the
    // rounding of the increment.
    for (cy::u32 step = 0; step <= 8; ++step) {
        const cy::f32 t = static_cast<cy::f32>(step) * 0.25f;
        CY_REQUIRE(cy::math::nearly_equal(curve.evaluate(t), t, 1e-5f));
        CY_REQUIRE(cy::math::nearly_equal(curve.derivative(t), 1.0f, 1e-4f));
    }
}

CY_TEST_CASE("Curve: extrapolation modes behave as named") {
    cy::Curve curve;
    curve.add_key(cy::CurveKey{0.0f, 0.0f, 2.0f, 2.0f});
    curve.add_key(cy::CurveKey{1.0f, 1.0f, 2.0f, 2.0f});

    // Clamp is the default and is the only mode that cannot produce a value the author never saw.
    CY_CHECK_EQ(curve.evaluate(-5.0f), 0.0f);
    CY_CHECK_EQ(curve.evaluate(5.0f), 1.0f);

    curve.set_extrapolation(cy::CurveExtrapolation::Linear, cy::CurveExtrapolation::Linear);
    CY_CHECK_CLOSE(curve.evaluate(2.0f), 1.0f + 2.0f, 1e-5f);
    CY_CHECK_CLOSE(curve.evaluate(-1.0f), 0.0f - 2.0f, 1e-5f);

    curve.set_extrapolation(cy::CurveExtrapolation::Repeat, cy::CurveExtrapolation::Repeat);
    CY_CHECK_CLOSE(curve.evaluate(2.5f), curve.evaluate(0.5f), 1e-5f);
    CY_CHECK_CLOSE(curve.evaluate(-0.5f), curve.evaluate(0.5f), 1e-5f);

    curve.set_extrapolation(cy::CurveExtrapolation::PingPong, cy::CurveExtrapolation::PingPong);
    // One period past the end, running backwards: 1.25 mirrors to 0.75.
    CY_CHECK_CLOSE(curve.evaluate(1.25f), curve.evaluate(0.75f), 1e-5f);
    CY_CHECK_CLOSE(curve.evaluate(2.25f), curve.evaluate(0.25f), 1e-5f);
}

CY_TEST_CASE("Curve3D: the spline passes through its control points") {
    const cy::Vec3 points[4] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 0.0f}, {4.0f, 1.0f, 0.0f}, {6.0f, 3.0f, 0.0f}};
    cy::Curve3D curve;
    curve.set_points(points, 4);
    CY_CHECK_EQ(curve.point_count(), 4u);

    // Catmull-Rom is an interpolating spline: every control point is on the curve, at the parameter
    // that names its segment boundary.
    for (cy::usize i = 0; i < 4; ++i) {
        const cy::f32 t = static_cast<cy::f32>(i) / 3.0f;
        CY_REQUIRE(nearly_equal(curve.sample(t), points[i], 1e-4f));
    }

    // The tangent points forward along the path.
    CY_CHECK(dot(curve.tangent(0.5f), points[3] - points[0]) > 0.0f);
}

CY_TEST_CASE("Curve3D: sampling by distance is constant speed") {
    // Control points deliberately unevenly spaced: the first segment is one metre long and the
    // second is ten. Sampling by *parameter* therefore covers ten times as much ground per unit of
    // t in the second segment as in the first, which is the artifact the arc-length table removes.
    const cy::Vec3 points[4] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {11.0f, 0.0f, 0.0f}, {12.0f, 0.0f, 0.0f}};
    cy::Curve3D curve;
    curve.set_points(points, 4);

    // The accessors report rather than answering from a stale table.
    CY_CHECK_FALSE(curve.is_baked());
    CY_CHECK_FALSE(curve.length().has_value());
    CY_CHECK_FALSE(curve.sample_by_distance(1.0f).has_value());

    CY_REQUIRE(curve.bake(32).has_value());
    CY_CHECK(curve.is_baked());

    const cy::Expected<cy::f32, cy::Error> total = curve.length();
    CY_REQUIRE(total.has_value());
    CY_CHECK_CLOSE(*total, 12.0f, 0.1f);  // relative: within a metre of the polyline's 12

    // Step along the curve in equal distances and measure the step lengths. Constant speed means
    // every step covers the same ground; a parameter sweep over this curve would vary by 10x.
    constexpr cy::usize kSteps = 40;
    cy::f32 shortest = cy::math::kInfinity;
    cy::f32 longest = 0.0f;
    cy::Vec3 previous = *curve.sample_by_distance(0.0f);
    for (cy::usize i = 1; i <= kSteps; ++i) {
        const cy::f32 travelled = *total * static_cast<cy::f32>(i) / static_cast<cy::f32>(kSteps);
        const cy::Expected<cy::Vec3, cy::Error> here = curve.sample_by_distance(travelled);
        CY_REQUIRE(here.has_value());
        const cy::f32 step = distance(previous, *here);
        shortest = cy::math::min(shortest, step);
        longest = cy::math::max(longest, step);
        previous = *here;
    }
    // Within a few percent of each other. The residual spread is the piecewise-linear table's
    // approximation error, not a change of speed.
    CY_CHECK(longest / shortest < 1.25f);

    // The same measurement over the raw parameter, to show the artifact is real and that the table
    // is what removes it.
    cy::f32 parameter_shortest = cy::math::kInfinity;
    cy::f32 parameter_longest = 0.0f;
    previous = curve.sample(0.0f);
    for (cy::usize i = 1; i <= kSteps; ++i) {
        const cy::Vec3 here = curve.sample(static_cast<cy::f32>(i) / static_cast<cy::f32>(kSteps));
        const cy::f32 step = distance(previous, here);
        parameter_shortest = cy::math::min(parameter_shortest, step);
        parameter_longest = cy::math::max(parameter_longest, step);
        previous = here;
    }
    CY_CHECK(parameter_longest / parameter_shortest > 3.0f);

    // The ends are clamped rather than extrapolated.
    CY_CHECK(nearly_equal(*curve.sample_by_distance(-1.0f), curve.sample(0.0f), 1e-4f));
    CY_CHECK(nearly_equal(*curve.sample_by_distance(*total + 5.0f), curve.sample(1.0f), 1e-4f));

    // Adding a point invalidates the table rather than leaving a stale one behind.
    curve.add_point(cy::Vec3{13.0f, 0.0f, 0.0f});
    CY_CHECK_FALSE(curve.is_baked());
}

CY_TEST_CASE("Curve2D: the plane form agrees with the 3D one") {
    const cy::Vec2 points[4] = {{0.0f, 0.0f}, {1.0f, 1.0f}, {3.0f, 0.0f}, {4.0f, 2.0f}};
    cy::Curve2D curve;
    curve.set_points(points, 4);
    CY_CHECK_EQ(curve.point_count(), 4u);
    CY_CHECK(nearly_equal(curve.sample(0.0f), points[0], 1e-4f));
    CY_CHECK(nearly_equal(curve.sample(1.0f), points[3], 1e-4f));
    CY_REQUIRE(curve.bake(16).has_value());
    CY_REQUIRE(curve.length().has_value());
    CY_CHECK(*curve.length() > 4.0f);
    CY_REQUIRE(curve.sample_by_distance(1.0f).has_value());
}

CY_TEST_CASE("Gradient: colour and alpha are keyed independently") {
    cy::Gradient gradient;
    gradient.add_color_key(0.0f, cy::colors::kBlack);
    gradient.add_color_key(1.0f, cy::colors::kWhite);
    gradient.add_alpha_key(0.0f, 1.0f);
    gradient.add_alpha_key(1.0f, 0.0f);
    CY_CHECK_EQ(gradient.color_key_count(), 2u);
    CY_CHECK_EQ(gradient.alpha_key_count(), 2u);

    const cy::Color middle = gradient.evaluate(0.5f);
    CY_CHECK_CLOSE(middle.r, 0.5f, 1e-5f);
    // Alpha runs the other way, which is the point of keying it separately: a fade-out over a ramp
    // that is not moving.
    CY_CHECK_CLOSE(middle.a, 0.5f, 1e-5f);

    // Outside the key range holds the endpoints.
    CY_CHECK(gradient.evaluate(-1.0f) == cy::Color{0.0f, 0.0f, 0.0f, 1.0f});
    CY_CHECK(gradient.evaluate(2.0f) == cy::Color{1.0f, 1.0f, 1.0f, 0.0f});

    // An empty gradient is opaque white: the neutral value for a multiply, so a missing gradient
    // looks like a missing gradient rather than like a black hole.
    const cy::Gradient empty;
    CY_CHECK(empty.evaluate(0.5f) == cy::colors::kWhite);
}

CY_TEST_CASE("Easing: every family and mode is anchored at 0 and 1") {
    // The property that has to hold for all forty-four entries, and the one that a transcription
    // error in any single family breaks.
    for (cy::u32 kind = 0; kind < static_cast<cy::u32>(cy::ease::Kind::kCount); ++kind) {
        for (cy::u32 mode = 0; mode < static_cast<cy::u32>(cy::ease::Mode::kCount); ++mode) {
            const auto k = static_cast<cy::ease::Kind>(kind);
            const auto m = static_cast<cy::ease::Mode>(mode);
            CY_REQUIRE(cy::math::nearly_equal(cy::ease::evaluate(k, m, 0.0f), 0.0f, 1e-5f));
            CY_REQUIRE(cy::math::nearly_equal(cy::ease::evaluate(k, m, 1.0f), 1.0f, 1e-5f));
            // The parameter is clamped, so an extrapolated t cannot produce an undefined tail from
            // expo or elastic.
            CY_REQUIRE(cy::math::nearly_equal(cy::ease::evaluate(k, m, -1.0f), 0.0f, 1e-5f));
            CY_REQUIRE(cy::math::nearly_equal(cy::ease::evaluate(k, m, 2.0f), 1.0f, 1e-5f));
            CY_REQUIRE(cy::ease::function(k, m) != nullptr);
        }
    }
}

CY_TEST_CASE("Easing: the four modes are reflections of one family") {
    // `Out` is `In` mirrored through both axes. It is derived that way in easing.cpp, and this
    // locks the derivation in: the classic bug is a family whose Out form was transcribed from a
    // different source and is not its own In form's mirror, and the way that bug arrives is
    // somebody replacing the generic reflection with a hand-written table.
    for (cy::u32 kind = 0; kind < static_cast<cy::u32>(cy::ease::Kind::kCount); ++kind) {
        const auto k = static_cast<cy::ease::Kind>(kind);
        // Eleven samples across [0, 1] inclusive. Accumulating `t += 0.1f` in f32 overshoots 1.0f
        // on the eleventh step and silently drops the endpoint, which is the sample most likely to
        // catch a mirrored family transcribed from the wrong source.
        for (cy::u32 step = 0; step <= 10; ++step) {
            const cy::f32 t = static_cast<cy::f32>(step) * 0.1f;
            const cy::f32 in_value = cy::ease::evaluate(k, cy::ease::Mode::In, 1.0f - t);
            const cy::f32 out_value = cy::ease::evaluate(k, cy::ease::Mode::Out, t);
            CY_REQUIRE(cy::math::nearly_equal(out_value, 1.0f - in_value, 1e-4f));
        }
        // In-out is symmetric about its midpoint, and passes through 0.5 there.
        CY_REQUIRE(cy::math::nearly_equal(cy::ease::evaluate(k, cy::ease::Mode::InOut, 0.5f), 0.5f,
                                          1e-5f));
    }

    // Linear ignores the mode entirely.
    for (cy::u32 mode = 0; mode < static_cast<cy::u32>(cy::ease::Mode::kCount); ++mode) {
        const auto m = static_cast<cy::ease::Mode>(mode);
        CY_CHECK_CLOSE(cy::ease::evaluate(cy::ease::Kind::Linear, m, 0.37f), 0.37f, 1e-6f);
    }

    // Back and elastic overshoot on purpose; clamping them here would remove the effect they exist
    // for, so the table does not clamp and this asserts that it does not.
    CY_CHECK(cy::ease::evaluate(cy::ease::Kind::Back, cy::ease::Mode::In, 0.3f) < 0.0f);
    CY_CHECK(cy::ease::evaluate(cy::ease::Kind::Back, cy::ease::Mode::Out, 0.7f) > 1.0f);

    // The names are non-null for every enumerator, which is what an editor drop-down needs.
    CY_CHECK(cy::ease::kind_name(cy::ease::Kind::Bounce) != nullptr);
    CY_CHECK(cy::ease::mode_name(cy::ease::Mode::OutIn) != nullptr);
}

CY_TEST_CASE("Easing: an out-of-range enumerator degrades to linear rather than crashing") {
    // An out-of-range enumerator is a programmer error, but the shipping response is a visible
    // oddity rather than a null function pointer call.
    const auto bogus_kind = static_cast<cy::ease::Kind>(999u);
    const auto bogus_mode = static_cast<cy::ease::Mode>(999u);
    CY_CHECK(cy::ease::function(bogus_kind, cy::ease::Mode::In) != nullptr);
    CY_CHECK_CLOSE(cy::ease::evaluate(bogus_kind, bogus_mode, 0.25f), 0.25f, 1e-6f);
    CY_CHECK(std::string_view(cy::ease::kind_name(bogus_kind)) == "unknown");
}
