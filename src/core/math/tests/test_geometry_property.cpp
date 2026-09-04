// The geometry routines' defining properties, over randomised input. Task 3.1.5.
//
// The hand-written cases in tests/test_polygon_ops.cpp check answers a reader can verify by eye.
// These check the *properties* — the statements that are true of every correct answer and of no
// incorrect one — over hundreds of shapes a human would not think to draw:
//
//   convex hull        every input point is on or behind every face, and every face faces outward.
//                      An algorithm that loses a point still produces a closed, plausible solid.
//   Delaunay           no point lies inside any triangle's circumcircle, and the triangles tile the
//                      point set's hull exactly. The first is what makes it Delaunay rather than
//                      merely a triangulation; the second catches an overlap or a gap.
//   booleans           |A ∪ B| + |A ∩ B| = |A| + |B|, over random convex pairs. This is the one
//                      identity that ties all three operations together, so a systematic error in
//                      any of them breaks it.
//   offsetting         every edge of the result is exactly the offset distance from the line of the
//                      edge it came from.
//
// This is an integration suite because of the corpus, not the subject: a unit test has a
// millisecond, and a corpus small enough to fit in one would not exercise a case the deterministic
// tests do not already cover. The seeds are fixed, so a failure here is reproducible.

#include <cy/core/math/math.h>

#include <cy/test/test.h>

#include "approx.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

/// Positive when `d` is strictly inside the circumcircle of the counter-clockwise triangle
/// (a, b, c). Written out here rather than reached for in the implementation on purpose: a test
/// that calls the code under test to decide whether the code under test is right proves nothing.
[[nodiscard]] double incircle(cy::Vec2 a, cy::Vec2 b, cy::Vec2 c, cy::Vec2 d) {
    const double adx = static_cast<double>(a.x) - static_cast<double>(d.x);
    const double ady = static_cast<double>(a.y) - static_cast<double>(d.y);
    const double bdx = static_cast<double>(b.x) - static_cast<double>(d.x);
    const double bdy = static_cast<double>(b.y) - static_cast<double>(d.y);
    const double cdx = static_cast<double>(c.x) - static_cast<double>(d.x);
    const double cdy = static_cast<double>(c.y) - static_cast<double>(d.y);
    const double a_lift = (adx * adx) + (ady * ady);
    const double b_lift = (bdx * bdx) + (bdy * bdy);
    const double c_lift = (cdx * cdx) + (cdy * cdy);
    return (adx * ((bdy * c_lift) - (b_lift * cdy))) - (ady * ((bdx * c_lift) - (b_lift * cdx))) +
           (a_lift * ((bdx * cdy) - (bdy * cdx)));
}

[[nodiscard]] cy::f32 polygon_area(const cy::Vec2* vertices, cy::usize count) {
    return std::fabs(cy::geom::polygon_signed_area2(vertices, count)) * 0.5f;
}

/// A simple, strictly convex polygon: `count` points on an ellipse at strictly increasing angles.
/// Convex rather than merely simple because the boolean identity below assumes the results have no
/// holes, and the union of two convex polygons never does.
[[nodiscard]] std::vector<cy::Vec2> convex_polygon(cy::Random& random, cy::usize count,
                                                   cy::Vec2 centre, cy::f32 radius) {
    std::vector<cy::f32> angles;
    angles.reserve(count);
    for (cy::usize i = 0; i < count; ++i) {
        angles.push_back(random.next_float_in(0.0f, cy::math::kTwoPi));
    }
    std::ranges::sort(angles);

    std::vector<cy::Vec2> polygon;
    polygon.reserve(count);
    for (const cy::f32 angle : angles) {
        polygon.push_back(cy::Vec2{centre.x + (radius * std::cos(angle)),
                                   centre.y + (radius * 0.75f * std::sin(angle))});
    }
    return polygon;
}

/// Total area over every contour of a boolean result.
[[nodiscard]] cy::f32 total_area(const std::vector<cy::Vec2>& vertices,
                                 const std::vector<cy::u32>& contours, cy::usize contour_count) {
    cy::f32 total = 0.0f;
    cy::usize at = 0;
    for (cy::usize c = 0; c < contour_count; ++c) {
        total += polygon_area(vertices.data() + at, contours[c]);
        at += contours[c];
    }
    return total;
}

}  // namespace

CY_TEST_CASE("convex_hull_3d: every point is behind every face of the hull it produced") {
    constexpr cy::usize kPointCount = 800;
    cy::Random random(0x51DEC0DEull);
    std::vector<cy::Vec3> points;
    points.reserve(kPointCount);
    for (cy::usize i = 0; i < kPointCount; ++i) {
        // A mixture of surface and interior points: the surface ones are the hull, the interior
        // ones are what the visibility test must reject, and a hull built from only one kind would
        // not exercise both halves of the loop.
        const cy::Vec3 direction = random.on_unit_sphere();
        const cy::f32 radius = i % 3 == 0 ? 5.0f : random.next_float_in(0.0f, 5.0f);
        points.push_back(direction * radius);
    }

    // Sized from the constant rather than from `points.size()`: a length the compiler can see is
    // what keeps GCC's allocation-size analysis quiet about a vector whose extent it cannot bound.
    std::vector<cy::u32> indices(6 * kPointCount);
    const cy::Expected<cy::usize, cy::Error> written = cy::geom::convex_hull_3d(
        points.data(), points.size(), 1e-5f, indices.data(), indices.size());
    CY_REQUIRE(written.has_value());
    CY_REQUIRE(*written % 3 == 0);
    const cy::usize face_count = *written / 3;
    // Euler: a triangulated hull over v vertices has exactly 2v - 4 faces, so the face count bounds
    // the vertex count. Anything wildly larger means the stitch is producing degenerate faces.
    CY_CHECK(face_count >= 4);
    CY_CHECK(face_count <= 2 * kPointCount);

    // The property. `tolerance` is generous in absolute terms — the cloud is 10 m across — because
    // the hull's planes are recomputed from f32 inputs and a point exactly on a face is the
    // ordinary case here, not the exception.
    const cy::f32 tolerance = 1e-3f;
    cy::usize outside = 0;
    for (cy::usize f = 0; f < face_count; ++f) {
        const cy::Vec3 a = points[indices[3 * f]];
        const cy::Vec3 b = points[indices[(3 * f) + 1]];
        const cy::Vec3 c = points[indices[(3 * f) + 2]];
        const cy::Vec3 normal = cy::normalize(cy::cross(b - a, c - a));
        for (const cy::Vec3& p : points) {
            if (cy::dot(normal, p - a) > tolerance) {
                ++outside;
            }
        }
    }
    CY_CHECK_EQ(outside, cy::usize{0});
}

CY_TEST_CASE("convex_hull_3d: points on a sphere are all on the hull") {
    // Every point of a strictly convex set is a vertex of its hull, so this is the counting half of
    // the property above: the previous test catches a hull that is too small, this one catches a
    // hull that dropped a vertex while staying closed.
    constexpr cy::usize kPointCount = 300;
    cy::Random random(0xBEEFu);
    std::vector<cy::Vec3> points;
    points.reserve(kPointCount);
    for (cy::usize i = 0; i < kPointCount; ++i) {
        points.push_back(random.on_unit_sphere() * 2.0f);
    }

    std::vector<cy::u32> indices(6 * kPointCount);
    const cy::Expected<cy::usize, cy::Error> written = cy::geom::convex_hull_3d(
        points.data(), points.size(), 1e-6f, indices.data(), indices.size());
    CY_REQUIRE(written.has_value());

    std::vector<bool> used(points.size(), false);
    for (cy::usize i = 0; i < *written; ++i) {
        used[indices[i]] = true;
    }
    const cy::usize on_hull = static_cast<cy::usize>(std::count(used.begin(), used.end(), true));
    CY_CHECK_EQ(on_hull, kPointCount);
    // 2v - 4 faces for v vertices, exactly, when no three points are coplanar with a fourth.
    CY_CHECK_EQ(*written / 3, (2 * kPointCount) - 4);
}

CY_TEST_CASE("triangulate_delaunay: no point lies inside any triangle's circumcircle") {
    constexpr cy::usize kPointCount = 250;
    cy::Random random(0xD3A17Au);
    std::vector<cy::Vec2> points;
    points.reserve(kPointCount);
    for (cy::usize i = 0; i < kPointCount; ++i) {
        points.push_back(
            cy::Vec2{random.next_float_in(-50.0f, 50.0f), random.next_float_in(-50.0f, 50.0f)});
    }

    std::vector<cy::u32> indices(6 * kPointCount);
    const cy::Expected<cy::usize, cy::Error> written = cy::geom::triangulate_delaunay(
        points.data(), points.size(), indices.data(), indices.size());
    CY_REQUIRE(written.has_value());
    const cy::usize triangle_count = *written / 3;
    CY_CHECK(triangle_count > 0);
    CY_CHECK(triangle_count <= (2 * kPointCount) - 5);

    // The defining property. The threshold is not zero: four nearly co-circular points make the
    // determinant nearly zero, and the answer there is genuinely a tie rather than a violation. It
    // is scaled by the coordinate range to the fourth power, which is the determinant's own
    // dimension — a fixed epsilon would mean something different at every scale.
    const double scale = 100.0;
    const double threshold = 1e-9 * scale * scale * scale * scale;
    cy::usize violations = 0;
    for (cy::usize t = 0; t < triangle_count; ++t) {
        const cy::u32 ia = indices[3 * t];
        const cy::u32 ib = indices[(3 * t) + 1];
        const cy::u32 ic = indices[(3 * t) + 2];
        const cy::Vec2 triangle[3] = {points[ia], points[ib], points[ic]};
        CY_REQUIRE(cy::geom::polygon_signed_area2(triangle, 3) > 0.0f);
        for (cy::u32 p = 0; p < static_cast<cy::u32>(points.size()); ++p) {
            if (p == ia || p == ib || p == ic) {
                continue;
            }
            if (incircle(points[ia], points[ib], points[ic], points[p]) > threshold) {
                ++violations;
            }
        }
    }
    CY_CHECK_EQ(violations, cy::usize{0});
}

CY_TEST_CASE("triangulate_delaunay: the triangles tile the hull of the points exactly") {
    // An overlap or a gap changes the total area and nothing else does, so comparing the
    // triangulated area against the hull's area catches both with one number.
    constexpr cy::usize kPointCount = 200;
    cy::Random random(0x7A1Eu);
    std::vector<cy::Vec2> points;
    points.reserve(kPointCount);
    for (cy::usize i = 0; i < kPointCount; ++i) {
        points.push_back(
            cy::Vec2{random.next_float_in(-20.0f, 20.0f), random.next_float_in(-20.0f, 20.0f)});
    }

    std::vector<cy::u32> indices(6 * kPointCount);
    const cy::Expected<cy::usize, cy::Error> written = cy::geom::triangulate_delaunay(
        points.data(), points.size(), indices.data(), indices.size());
    CY_REQUIRE(written.has_value());

    double triangulated = 0.0;
    for (cy::usize t = 0; t < *written; t += 3) {
        const cy::Vec2 triangle[3] = {points[indices[t]], points[indices[t + 1]],
                                      points[indices[t + 2]]};
        triangulated += static_cast<double>(polygon_area(triangle, 3));
    }

    std::vector<cy::u32> hull(kPointCount + 1);
    std::vector<cy::u32> scratch(kPointCount);
    const cy::Expected<cy::usize, cy::Error> hull_size = cy::geom::convex_hull_2d(
        points.data(), points.size(), hull.data(), hull.size(), scratch.data());
    CY_REQUIRE(hull_size.has_value());
    std::vector<cy::Vec2> hull_points;
    hull_points.reserve(*hull_size);
    for (cy::usize i = 0; i < *hull_size; ++i) {
        hull_points.push_back(points[hull[i]]);
    }

    CY_CHECK_CLOSE(triangulated, static_cast<double>(polygon_area(hull_points.data(), *hull_size)),
                   1e-4);
}

CY_TEST_CASE("polygon_boolean: union plus intersection is the sum of the parts") {
    cy::Random random(0xB001Eu);
    std::vector<cy::Vec2> vertices(4096);
    std::vector<cy::u32> contours(64);

    cy::usize compared = 0;
    cy::usize refused = 0;
    for (cy::usize trial = 0; trial < 200; ++trial) {
        const cy::usize subject_count = 3 + random.next_u32_below(6);
        const cy::usize clip_count = 3 + random.next_u32_below(6);
        const std::vector<cy::Vec2> subject = convex_polygon(
            random, subject_count, cy::Vec2{0.0f, 0.0f}, random.next_float_in(2.0f, 6.0f));
        const std::vector<cy::Vec2> clip = convex_polygon(
            random, clip_count,
            cy::Vec2{random.next_float_in(-4.0f, 4.0f), random.next_float_in(-4.0f, 4.0f)},
            random.next_float_in(2.0f, 6.0f));

        const cy::Expected<cy::geom::BooleanResult, cy::Error> united = cy::geom::polygon_boolean(
            cy::geom::BooleanOp::Union, subject.data(), subject.size(), clip.data(), clip.size(),
            vertices.data(), vertices.size(), contours.data(), contours.size());
        if (!united) {
            // A refusal is a legitimate outcome — the boundaries touched — and is not a failure of
            // the identity. It is counted so that a run in which *everything* was refused cannot
            // pass by having compared nothing.
            CY_CHECK_EQ(united.error().code, cy::ErrorCode::Unsupported);
            ++refused;
            continue;
        }
        const cy::f32 union_area = total_area(vertices, contours, united->contour_count);

        const cy::Expected<cy::geom::BooleanResult, cy::Error> intersected =
            cy::geom::polygon_boolean(cy::geom::BooleanOp::Intersection, subject.data(),
                                      subject.size(), clip.data(), clip.size(), vertices.data(),
                                      vertices.size(), contours.data(), contours.size());
        CY_REQUIRE(intersected.has_value());
        const cy::f32 intersection_area =
            total_area(vertices, contours, intersected->contour_count);

        const cy::f32 subject_area = polygon_area(subject.data(), subject.size());
        const cy::f32 clip_area = polygon_area(clip.data(), clip.size());

        // The identity, at a tolerance relative to the areas involved rather than absolute: these
        // are f32 areas of shapes several metres across, accumulated over a dozen vertices.
        CY_CHECK_CLOSE(union_area + intersection_area, subject_area + clip_area, 1e-4f);

        // And the difference, which the identity above does not constrain. A difference that would
        // be an annulus is refused, which happens whenever the clip is strictly inside.
        const cy::Expected<cy::geom::BooleanResult, cy::Error> difference =
            cy::geom::polygon_boolean(cy::geom::BooleanOp::Difference, subject.data(),
                                      subject.size(), clip.data(), clip.size(), vertices.data(),
                                      vertices.size(), contours.data(), contours.size());
        if (difference) {
            const cy::f32 difference_area =
                total_area(vertices, contours, difference->contour_count);
            CY_CHECK_CLOSE(difference_area + intersection_area, subject_area, 1e-4f);
        } else {
            CY_CHECK_EQ(difference.error().code, cy::ErrorCode::Unsupported);
        }
        ++compared;
    }

    // Random convex polygons in general position almost never touch, so a run where most pairs were
    // refused means the degeneracy test has become too eager, which is a real regression and an
    // invisible one: every remaining test would still pass.
    CY_CHECK(compared > 150);
    CY_CHECK(refused < 20);
}

CY_TEST_CASE(
    "offset_polygon: every result edge is the offset distance from the edge it came from") {
    cy::Random random(0x0FF5E7u);
    std::vector<cy::Vec2> out(64);

    for (cy::usize trial = 0; trial < 100; ++trial) {
        const cy::usize count = 3 + random.next_u32_below(8);
        const std::vector<cy::Vec2> polygon =
            convex_polygon(random, count, cy::Vec2{}, random.next_float_in(2.0f, 8.0f));
        if (cy::geom::polygon_signed_area2(polygon.data(), polygon.size()) <= 0.0f) {
            continue;  // Two coincident angles produced a degenerate ring; not this test's subject.
        }
        const cy::f32 distance = random.next_float_in(0.05f, 0.4f);

        const cy::Expected<cy::usize, cy::Error> written =
            cy::geom::offset_polygon(polygon.data(), polygon.size(), distance,
                                     cy::geom::JoinStyle::Miter, 8.0f, out.data(), out.size());
        if (!written) {
            CY_CHECK_EQ(written.error().code, cy::ErrorCode::InvalidArgument);
            continue;
        }

        // Growing a convex polygon by d adds exactly d to the distance from its interior to every
        // one of its edge lines. Checking the *lines* rather than the vertices is what makes this a
        // property and not a restatement of the implementation: it holds for a miter, a bevel, and
        // any join anybody adds later.
        const cy::f32 area_before = polygon_area(polygon.data(), polygon.size());
        const cy::f32 area_after = polygon_area(out.data(), *written);
        CY_CHECK(area_after > area_before);

        for (cy::usize i = 0; i < polygon.size(); ++i) {
            const cy::Vec2 a = polygon[i];
            const cy::Vec2 b = polygon[(i + 1) % polygon.size()];
            const cy::Vec2 edge = b - a;
            const cy::f32 edge_length = cy::length(edge);
            if (edge_length < 1e-4f) {
                continue;
            }
            const cy::Vec2 outward{edge.y / edge_length, -edge.x / edge_length};

            // The furthest the offset polygon reaches along this edge's outward normal is exactly
            // the original polygon's own reach plus the offset — no more, because the offset is a
            // parallel move, and no less, because the corner join keeps the edge's own endpoints.
            cy::f32 before = -1e30f;
            for (const cy::Vec2& p : polygon) {
                before = cy::math::max(before, cy::dot(outward, p - a));
            }
            cy::f32 after = -1e30f;
            for (cy::usize v = 0; v < *written; ++v) {
                after = cy::math::max(after, cy::dot(outward, out[v] - a));
            }
            CY_CHECK_CLOSE(after, before + distance, 1e-3f);
        }
    }
}
