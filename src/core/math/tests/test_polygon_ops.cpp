// The four geometry routines with genuine degenerate cases: the 3D convex hull, Delaunay
// triangulation, polygon booleans and polygon offsetting. Task 3.1.5.
//
// `core-math` — "Geometry utilities" names all four. Each is tested on a shape whose answer can be
// written down by hand, and then on the degeneracy it refuses, because the refusal is half of what
// each of these routines promises. A boolean operation that silently drops a contour when two edges
// are collinear passes every test that only checks the well-behaved case, and the caller finds out
// three subsystems away.
//
// The randomised property tests — every triangle's circumcircle empty, every point inside the hull,
// |A ∪ B| = |A| + |B| − |A ∩ B| over hundreds of shapes — are in tests/test_geometry_property.cpp,
// which is an integration suite: they do not fit in a unit test's millisecond, and a corpus small
// enough to fit would not catch anything the hand-written cases here do not.

#include <cy/core/math/math.h>

#include <cy/test/test.h>

#include "approx.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

/// The area of a polygon given as a run of vertices, unsigned. Booleans are checked by area
/// because area is the property that survives a different but equally correct vertex ordering:
/// asserting the vertices themselves would test which crossing the walk happened to start from.
[[nodiscard]] cy::f32 polygon_area(const cy::Vec2* vertices, cy::usize count) {
    return std::fabs(cy::geom::polygon_signed_area2(vertices, count)) * 0.5f;
}

/// The eight corners of an axis-aligned box, in an order that is deliberately not the hull's.
[[nodiscard]] std::vector<cy::Vec3> box_corners(cy::f32 half) {
    return {
        cy::Vec3{-half, -half, -half}, cy::Vec3{half, -half, -half}, cy::Vec3{half, half, -half},
        cy::Vec3{-half, half, -half},  cy::Vec3{-half, -half, half}, cy::Vec3{half, -half, half},
        cy::Vec3{half, half, half},    cy::Vec3{-half, half, half},
    };
}

/// Every face of a closed hull, seen from outside, must have the hull's interior behind it. The
/// centroid stands in for the interior: it is inside any convex body.
[[nodiscard]] bool faces_point_outward(const std::vector<cy::Vec3>& points, const cy::u32* indices,
                                       cy::usize index_count) {
    cy::Vec3 centroid{};
    for (const cy::Vec3& p : points) {
        centroid += p;
    }
    centroid = centroid / static_cast<cy::f32>(points.size());

    for (cy::usize i = 0; i < index_count; i += 3) {
        const cy::Vec3 a = points[indices[i]];
        const cy::Vec3 b = points[indices[i + 1]];
        const cy::Vec3 c = points[indices[i + 2]];
        const cy::Vec3 normal = cy::cross(b - a, c - a);
        if (cy::dot(normal, centroid - a) > 0.0f) {
            return false;
        }
    }
    return true;
}

}  // namespace

// --- 3D convex hull -------------------------------------------------------------------------

CY_TEST_CASE("convex_hull_3d: a box hulls to twelve outward triangles over its eight corners") {
    const std::vector<cy::Vec3> corners = box_corners(1.0f);
    cy::u32 indices[64];
    const cy::Expected<cy::usize, cy::Error> written =
        cy::geom::convex_hull_3d(corners.data(), corners.size(), 1e-5f, indices, 64);
    CY_REQUIRE(written.has_value());

    // A triangulated hull over eight vertices has 2n - 4 = 12 faces, and every corner of a box is
    // on it — a hull that quietly dropped one would still be closed and would still look right.
    CY_CHECK_EQ(*written, cy::usize{36});
    CY_CHECK(faces_point_outward(corners, indices, *written));

    bool used[8] = {};
    for (cy::usize i = 0; i < *written; ++i) {
        CY_REQUIRE(indices[i] < 8);
        // Bounded by the REQUIRE above; the analyzer does not model the harness's abort.
        // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound)
        used[indices[i]] = true;
    }
    for (const bool corner_used : used) {
        CY_CHECK(corner_used);
    }
}

CY_TEST_CASE("convex_hull_3d: interior points do not reach the hull") {
    std::vector<cy::Vec3> points = box_corners(1.0f);
    // Three points strictly inside, and one exactly on a face — the second is the interesting one,
    // because a face point is at distance zero and the tolerance is what decides it.
    points.push_back(cy::Vec3{0.0f, 0.0f, 0.0f});
    points.push_back(cy::Vec3{0.25f, -0.5f, 0.1f});
    points.push_back(cy::Vec3{0.0f, 0.0f, 1.0f});

    cy::u32 indices[128];
    const cy::Expected<cy::usize, cy::Error> written =
        cy::geom::convex_hull_3d(points.data(), points.size(), 1e-5f, indices, 128);
    CY_REQUIRE(written.has_value());
    CY_CHECK_EQ(*written, cy::usize{36});
    for (cy::usize i = 0; i < *written; ++i) {
        CY_CHECK(indices[i] < 8);
    }
}

CY_TEST_CASE("convex_hull_3d: a cloud with no volume is refused, not flattened") {
    cy::u32 indices[64];

    // Coplanar: a 3D hull of a flat set is a 2D hull in a plane, and returning a zero-thickness
    // shell would make every consumer special-case it.
    const cy::Vec3 flat[] = {cy::Vec3{0.0f, 0.0f, 0.0f}, cy::Vec3{1.0f, 0.0f, 0.0f},
                             cy::Vec3{1.0f, 1.0f, 0.0f}, cy::Vec3{0.0f, 1.0f, 0.0f},
                             cy::Vec3{0.5f, 0.5f, 0.0f}};
    const cy::Expected<cy::usize, cy::Error> coplanar =
        cy::geom::convex_hull_3d(flat, 5, 1e-5f, indices, 64);
    CY_REQUIRE_FALSE(coplanar.has_value());
    CY_CHECK_EQ(coplanar.error().code, cy::ErrorCode::InvalidArgument);

    // Collinear, and coincident, take the same path.
    const cy::Vec3 line[] = {cy::Vec3{0.0f, 0.0f, 0.0f}, cy::Vec3{1.0f, 1.0f, 1.0f},
                             cy::Vec3{2.0f, 2.0f, 2.0f}, cy::Vec3{3.0f, 3.0f, 3.0f}};
    CY_CHECK_FALSE(cy::geom::convex_hull_3d(line, 4, 1e-5f, indices, 64).has_value());

    // Fewer than four points cannot bound a volume at all.
    CY_CHECK_FALSE(cy::geom::convex_hull_3d(flat, 3, 1e-5f, indices, 64).has_value());

    // And the buffer contract: 6 * count, checked before anything is written.
    const std::vector<cy::Vec3> corners = box_corners(1.0f);
    const cy::Expected<cy::usize, cy::Error> cramped =
        cy::geom::convex_hull_3d(corners.data(), corners.size(), 1e-5f, indices, 12);
    CY_REQUIRE_FALSE(cramped.has_value());
    CY_CHECK_EQ(cramped.error().code, cy::ErrorCode::BufferTooSmall);
}

// --- Delaunay triangulation -----------------------------------------------------------------

CY_TEST_CASE("triangulate_delaunay: four corners of a square split into two triangles") {
    const cy::Vec2 square[] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{1.0f, 0.0f}, cy::Vec2{1.0f, 1.0f},
                               cy::Vec2{0.0f, 1.0f}};
    cy::u32 indices[64];
    const cy::Expected<cy::usize, cy::Error> written =
        cy::geom::triangulate_delaunay(square, 4, indices, 64);
    CY_REQUIRE(written.has_value());
    CY_CHECK_EQ(*written, cy::usize{6});

    // The two triangles tile the square exactly: any triangulation of it has area 1, and a
    // triangulation that overlapped itself or left a gap would not.
    cy::f32 total = 0.0f;
    for (cy::usize i = 0; i < *written; i += 3) {
        const cy::Vec2 triangle[3] = {square[indices[i]], square[indices[i + 1]],
                                      square[indices[i + 2]]};
        // Counter-clockwise, which the interface promises.
        CY_CHECK(cy::geom::polygon_signed_area2(triangle, 3) > 0.0f);
        total += polygon_area(triangle, 3);
    }
    CY_CHECK_CLOSE(total, 1.0f, 1e-5f);
}

CY_TEST_CASE("triangulate_delaunay: the diagonal splits the shorter way") {
    // The property that distinguishes Delaunay from any other triangulation, in its smallest
    // possible instance. A thin quadrilateral has two triangulations; only one has empty
    // circumcircles, and it is the one that does *not* use the long diagonal.
    const cy::Vec2 kite[] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{4.0f, 0.1f}, cy::Vec2{8.0f, 0.0f},
                             cy::Vec2{4.0f, -3.0f}};
    cy::u32 indices[64];
    const cy::Expected<cy::usize, cy::Error> written =
        cy::geom::triangulate_delaunay(kite, 4, indices, 64);
    CY_REQUIRE(written.has_value());
    CY_REQUIRE_EQ(*written, cy::usize{6});

    // The short diagonal is 1 → 3 (length ≈ 3.1); the long one is 0 → 2 (length 8). Exactly one of
    // them appears in a two-triangle triangulation, and Delaunay picks the short one.
    bool has_short_diagonal = false;
    bool has_long_diagonal = false;
    for (cy::usize i = 0; i < *written; i += 3) {
        for (cy::usize e = 0; e < 3; ++e) {
            const cy::u32 a = indices[i + e];
            const cy::u32 b = indices[i + ((e + 1) % 3)];
            has_short_diagonal = has_short_diagonal || (a == 1 && b == 3) || (a == 3 && b == 1);
            has_long_diagonal = has_long_diagonal || (a == 0 && b == 2) || (a == 2 && b == 0);
        }
    }
    CY_CHECK(has_short_diagonal);
    CY_CHECK_FALSE(has_long_diagonal);
}

CY_TEST_CASE("triangulate_delaunay: duplicates and collinear sets are refused") {
    cy::u32 indices[64];

    const cy::Vec2 duplicated[] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{1.0f, 0.0f}, cy::Vec2{1.0f, 0.0f},
                                   cy::Vec2{0.0f, 1.0f}};
    const cy::Expected<cy::usize, cy::Error> duplicate =
        cy::geom::triangulate_delaunay(duplicated, 4, indices, 64);
    CY_REQUIRE_FALSE(duplicate.has_value());
    CY_CHECK_EQ(duplicate.error().code, cy::ErrorCode::InvalidArgument);

    // Collinear points have no triangulation — not an empty one, none at all — and the difference
    // matters: an empty result would read as "these points are fine and produced nothing".
    const cy::Vec2 collinear[] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{1.0f, 1.0f}, cy::Vec2{2.0f, 2.0f},
                                  cy::Vec2{3.0f, 3.0f}};
    const cy::Expected<cy::usize, cy::Error> line =
        cy::geom::triangulate_delaunay(collinear, 4, indices, 64);
    CY_REQUIRE_FALSE(line.has_value());
    CY_CHECK_EQ(line.error().code, cy::ErrorCode::InvalidArgument);

    const cy::Vec2 square[] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{1.0f, 0.0f}, cy::Vec2{1.0f, 1.0f},
                               cy::Vec2{0.0f, 1.0f}};
    const cy::Expected<cy::usize, cy::Error> cramped =
        cy::geom::triangulate_delaunay(square, 4, indices, 8);
    CY_REQUIRE_FALSE(cramped.has_value());
    CY_CHECK_EQ(cramped.error().code, cy::ErrorCode::BufferTooSmall);
}

// --- Polygon booleans -----------------------------------------------------------------------

namespace {

/// Two 2×2 squares overlapping in a 1×1 corner: areas 4 and 4, intersection 1, so union 7 and
/// difference 3. Every number below is one of those four, which is what makes the expectations
/// checkable by eye rather than by rerunning the code that produced them.
constexpr cy::Vec2 kSquareA[4] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{2.0f, 0.0f}, cy::Vec2{2.0f, 2.0f},
                                  cy::Vec2{0.0f, 2.0f}};
constexpr cy::Vec2 kSquareB[4] = {cy::Vec2{1.0f, 1.0f}, cy::Vec2{3.0f, 1.0f}, cy::Vec2{3.0f, 3.0f},
                                  cy::Vec2{1.0f, 3.0f}};

struct BooleanOutput {
    cy::Vec2 vertices[64];
    cy::u32 contours[8] = {};
    cy::geom::BooleanResult result;

    [[nodiscard]] cy::f32 total_area() const {
        cy::f32 total = 0.0f;
        cy::usize at = 0;
        for (cy::usize c = 0; c < result.contour_count; ++c) {
            total += polygon_area(vertices + at, contours[c]);
            at += contours[c];
        }
        return total;
    }

    /// Every contour comes back counter-clockwise; the interface says so, so it is checked rather
    /// than assumed by the area helper's `fabs`.
    [[nodiscard]] bool all_counter_clockwise() const {
        cy::usize at = 0;
        for (cy::usize c = 0; c < result.contour_count; ++c) {
            if (cy::geom::polygon_signed_area2(vertices + at, contours[c]) <= 0.0f) {
                return false;
            }
            at += contours[c];
        }
        return true;
    }
};

[[nodiscard]] cy::Expected<cy::geom::BooleanResult, cy::Error> run_boolean(
    cy::geom::BooleanOp op, const cy::Vec2* subject, cy::usize subject_count, const cy::Vec2* clip,
    cy::usize clip_count, BooleanOutput& out) {
    cy::Expected<cy::geom::BooleanResult, cy::Error> result = cy::geom::polygon_boolean(
        op, subject, subject_count, clip, clip_count, out.vertices, 64, out.contours, 8);
    if (result) {
        out.result = *result;
    }
    return result;
}

}  // namespace

CY_TEST_CASE("polygon_boolean: two overlapping squares, all three operations") {
    BooleanOutput output;

    const cy::Expected<cy::geom::BooleanResult, cy::Error> intersection =
        run_boolean(cy::geom::BooleanOp::Intersection, kSquareA, 4, kSquareB, 4, output);
    CY_REQUIRE(intersection.has_value());
    CY_CHECK_EQ(output.result.contour_count, cy::usize{1});
    CY_CHECK_EQ(output.result.vertex_count, cy::usize{4});
    CY_CHECK_CLOSE(output.total_area(), 1.0f, 1e-5f);
    CY_CHECK(output.all_counter_clockwise());

    const cy::Expected<cy::geom::BooleanResult, cy::Error> united =
        run_boolean(cy::geom::BooleanOp::Union, kSquareA, 4, kSquareB, 4, output);
    CY_REQUIRE(united.has_value());
    CY_CHECK_EQ(output.result.contour_count, cy::usize{1});
    CY_CHECK_CLOSE(output.total_area(), 7.0f, 1e-5f);
    CY_CHECK(output.all_counter_clockwise());

    const cy::Expected<cy::geom::BooleanResult, cy::Error> difference =
        run_boolean(cy::geom::BooleanOp::Difference, kSquareA, 4, kSquareB, 4, output);
    CY_REQUIRE(difference.has_value());
    CY_CHECK_EQ(output.result.contour_count, cy::usize{1});
    CY_CHECK_CLOSE(output.total_area(), 3.0f, 1e-5f);
    CY_CHECK(output.all_counter_clockwise());
}

CY_TEST_CASE("polygon_boolean: an intersection can be two disjoint pieces") {
    // A U-shape and a bar across its prongs. The intersection is two separate squares, which is why
    // the interface returns a set of contours and not a polygon: a routine that returned one would
    // have to choose which half to lose.
    const cy::Vec2 u_shape[] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{3.0f, 0.0f}, cy::Vec2{3.0f, 3.0f},
                                cy::Vec2{2.0f, 3.0f}, cy::Vec2{2.0f, 1.0f}, cy::Vec2{1.0f, 1.0f},
                                cy::Vec2{1.0f, 3.0f}, cy::Vec2{0.0f, 3.0f}};
    const cy::Vec2 bar[] = {cy::Vec2{-1.0f, 2.0f}, cy::Vec2{4.0f, 2.0f}, cy::Vec2{4.0f, 2.5f},
                            cy::Vec2{-1.0f, 2.5f}};

    BooleanOutput output;
    const cy::Expected<cy::geom::BooleanResult, cy::Error> intersection =
        run_boolean(cy::geom::BooleanOp::Intersection, u_shape, 8, bar, 4, output);
    CY_REQUIRE(intersection.has_value());
    CY_CHECK_EQ(output.result.contour_count, cy::usize{2});
    // Two prongs, each 1 wide and 0.5 tall.
    CY_CHECK_CLOSE(output.total_area(), 1.0f, 1e-5f);
    CY_CHECK(output.all_counter_clockwise());
}

CY_TEST_CASE("polygon_boolean: polygons that never cross are answered by containment") {
    const cy::Vec2 outer[] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{10.0f, 0.0f}, cy::Vec2{10.0f, 10.0f},
                              cy::Vec2{0.0f, 10.0f}};
    const cy::Vec2 inner[] = {cy::Vec2{2.0f, 2.0f}, cy::Vec2{4.0f, 2.0f}, cy::Vec2{4.0f, 4.0f},
                              cy::Vec2{2.0f, 4.0f}};
    const cy::Vec2 elsewhere[] = {cy::Vec2{20.0f, 20.0f}, cy::Vec2{21.0f, 20.0f},
                                  cy::Vec2{21.0f, 21.0f}, cy::Vec2{20.0f, 21.0f}};
    BooleanOutput output;

    CY_REQUIRE(run_boolean(cy::geom::BooleanOp::Union, outer, 4, inner, 4, output).has_value());
    CY_CHECK_CLOSE(output.total_area(), 100.0f, 1e-4f);

    CY_REQUIRE(
        run_boolean(cy::geom::BooleanOp::Intersection, outer, 4, inner, 4, output).has_value());
    CY_CHECK_CLOSE(output.total_area(), 4.0f, 1e-5f);

    // Disjoint: an empty intersection is an answer, not a failure.
    CY_REQUIRE(
        run_boolean(cy::geom::BooleanOp::Intersection, outer, 4, elsewhere, 4, output).has_value());
    CY_CHECK_EQ(output.result.contour_count, cy::usize{0});

    // ...and their union is two contours.
    CY_REQUIRE(run_boolean(cy::geom::BooleanOp::Union, outer, 4, elsewhere, 4, output).has_value());
    CY_CHECK_EQ(output.result.contour_count, cy::usize{2});
    CY_CHECK_CLOSE(output.total_area(), 101.0f, 1e-4f);

    // A difference that would be an annulus is refused rather than silently filled in — this
    // interface cannot say "this contour is a hole in that one".
    const cy::Expected<cy::geom::BooleanResult, cy::Error> annulus =
        run_boolean(cy::geom::BooleanOp::Difference, outer, 4, inner, 4, output);
    CY_REQUIRE_FALSE(annulus.has_value());
    CY_CHECK_EQ(annulus.error().code, cy::ErrorCode::Unsupported);
}

CY_TEST_CASE("polygon_boolean: a touching boundary is refused by name") {
    BooleanOutput output;

    // Two identical squares: every edge pair is collinear and overlapping. Greiner-Hormann is
    // undefined here, and the whole point is that it says so instead of returning three vertices
    // of a four-vertex answer.
    const cy::Expected<cy::geom::BooleanResult, cy::Error> identical =
        run_boolean(cy::geom::BooleanOp::Union, kSquareA, 4, kSquareA, 4, output);
    CY_REQUIRE_FALSE(identical.has_value());
    CY_CHECK_EQ(identical.error().code, cy::ErrorCode::Unsupported);

    // Edge-to-edge contact, which is the same degeneracy in its most innocent-looking form: two
    // squares sharing a wall.
    const cy::Vec2 adjacent[] = {cy::Vec2{2.0f, 0.0f}, cy::Vec2{4.0f, 0.0f}, cy::Vec2{4.0f, 2.0f},
                                 cy::Vec2{2.0f, 2.0f}};
    CY_CHECK_FALSE(
        run_boolean(cy::geom::BooleanOp::Union, kSquareA, 4, adjacent, 4, output).has_value());

    // A vertex sitting exactly on the other polygon's edge.
    const cy::Vec2 touching[] = {cy::Vec2{1.0f, 2.0f}, cy::Vec2{3.0f, 3.0f}, cy::Vec2{1.5f, 4.0f}};
    CY_CHECK_FALSE(
        run_boolean(cy::geom::BooleanOp::Union, kSquareA, 4, touching, 3, output).has_value());
}

CY_TEST_CASE("polygon_boolean: the two output buffers are checked separately") {
    cy::Vec2 vertices[3];
    cy::u32 contours[4];
    const cy::Expected<cy::geom::BooleanResult, cy::Error> cramped_vertices =
        cy::geom::polygon_boolean(cy::geom::BooleanOp::Union, kSquareA, 4, kSquareB, 4, vertices, 3,
                                  contours, 4);
    CY_REQUIRE_FALSE(cramped_vertices.has_value());
    CY_CHECK_EQ(cramped_vertices.error().code, cy::ErrorCode::BufferTooSmall);

    cy::Vec2 roomy[64];
    const cy::Expected<cy::geom::BooleanResult, cy::Error> cramped_contours =
        cy::geom::polygon_boolean(cy::geom::BooleanOp::Union, kSquareA, 4, kSquareB, 4, roomy, 64,
                                  contours, 0);
    CY_REQUIRE_FALSE(cramped_contours.has_value());
    CY_CHECK_EQ(cramped_contours.error().code, cy::ErrorCode::BufferTooSmall);

    // And a polygon that is not one.
    cy::u32 sizes[4];
    CY_CHECK_FALSE(cy::geom::polygon_boolean(cy::geom::BooleanOp::Union, kSquareA, 2, kSquareB, 4,
                                             roomy, 64, sizes, 4)
                       .has_value());
}

// --- Polygon offsetting ---------------------------------------------------------------------

CY_TEST_CASE("offset_polygon: a square grows and shrinks by the offset on every side") {
    const cy::Vec2 square[] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{2.0f, 0.0f}, cy::Vec2{2.0f, 2.0f},
                               cy::Vec2{0.0f, 2.0f}};
    cy::Vec2 out[8];

    const cy::Expected<cy::usize, cy::Error> grown =
        cy::geom::offset_polygon(square, 4, 0.5f, cy::geom::JoinStyle::Miter, 4.0f, out, 8);
    CY_REQUIRE(grown.has_value());
    // A right-angled corner has a miter ratio of √2, well inside the limit, so four corners give
    // four vertices and the result is the 3 × 3 square: the numeric consequence of "outward".
    CY_REQUIRE_EQ(*grown, cy::usize{4});
    CY_CHECK_CLOSE(polygon_area(out, *grown), 9.0f, 1e-5f);
    CY_CHECK_CLOSE(out[0].x, -0.5f, 1e-5f);
    CY_CHECK_CLOSE(out[0].y, -0.5f, 1e-5f);

    const cy::Expected<cy::usize, cy::Error> shrunk =
        cy::geom::offset_polygon(square, 4, -0.5f, cy::geom::JoinStyle::Miter, 4.0f, out, 8);
    CY_REQUIRE(shrunk.has_value());
    CY_REQUIRE_EQ(*shrunk, cy::usize{4});
    CY_CHECK_CLOSE(polygon_area(out, *shrunk), 1.0f, 1e-5f);
}

CY_TEST_CASE("offset_polygon: a sharp corner bevels rather than growing a needle") {
    // A 20-degree wedge. Its miter ratio is 1/sin(10°) ≈ 5.76, so a limit of 4 must bevel it and a
    // limit of 8 must not — the two calls differ only in that number, and the vertex count is the
    // observable.
    const cy::f32 half_angle = cy::math::radians(10.0f);
    const cy::Vec2 wedge[] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{10.0f, -10.0f * std::tan(half_angle)},
                              cy::Vec2{10.0f, 10.0f * std::tan(half_angle)}};
    cy::Vec2 out[8];

    const cy::Expected<cy::usize, cy::Error> limited =
        cy::geom::offset_polygon(wedge, 3, 0.2f, cy::geom::JoinStyle::Miter, 4.0f, out, 8);
    CY_REQUIRE(limited.has_value());
    CY_CHECK_EQ(*limited, cy::usize{4});  // the tip bevelled, the two blunt corners mitered

    const cy::Expected<cy::usize, cy::Error> unlimited =
        cy::geom::offset_polygon(wedge, 3, 0.2f, cy::geom::JoinStyle::Miter, 8.0f, out, 8);
    CY_REQUIRE(unlimited.has_value());
    CY_CHECK_EQ(*unlimited, cy::usize{3});

    // Bevel style takes every convex corner, whatever the limit says.
    const cy::Expected<cy::usize, cy::Error> bevelled =
        cy::geom::offset_polygon(wedge, 3, 0.2f, cy::geom::JoinStyle::Bevel, 8.0f, out, 8);
    CY_REQUIRE(bevelled.has_value());
    CY_CHECK_EQ(*bevelled, cy::usize{6});
}

CY_TEST_CASE("offset_polygon: a reflex corner keeps its miter whatever the style") {
    // An L. Offsetting outward, the inner corner is reflex: its two offset edges cross, the
    // intersection is the only sensible point, and bevelling it would cut a notch out of the shape.
    const cy::Vec2 el[] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{3.0f, 0.0f}, cy::Vec2{3.0f, 1.0f},
                           cy::Vec2{1.0f, 1.0f}, cy::Vec2{1.0f, 3.0f}, cy::Vec2{0.0f, 3.0f}};
    cy::Vec2 out[12];

    const cy::Expected<cy::usize, cy::Error> bevelled =
        cy::geom::offset_polygon(el, 6, 0.25f, cy::geom::JoinStyle::Bevel, 4.0f, out, 12);
    CY_REQUIRE(bevelled.has_value());
    // Five convex corners bevel into two vertices each; the one reflex corner stays a single
    // miter. Ten plus one.
    CY_CHECK_EQ(*bevelled, cy::usize{11});

    const cy::Expected<cy::usize, cy::Error> mitered =
        cy::geom::offset_polygon(el, 6, 0.25f, cy::geom::JoinStyle::Miter, 4.0f, out, 12);
    CY_REQUIRE(mitered.has_value());
    CY_REQUIRE_EQ(*mitered, cy::usize{6});
    // Growing an L by d adds d to its outline on every side: the area grows by the perimeter times
    // d plus the corner areas. Rather than restate that formula, check the two numbers a reader can
    // verify: the shape still winds counter-clockwise, and it is strictly larger.
    CY_CHECK(cy::geom::polygon_signed_area2(out, *mitered) > 0.0f);
    CY_CHECK(polygon_area(out, *mitered) > polygon_area(el, 6));
}

CY_TEST_CASE("offset_polygon: winding, capacity and degenerate edges are refused") {
    const cy::Vec2 clockwise[] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{0.0f, 2.0f}, cy::Vec2{2.0f, 2.0f},
                                  cy::Vec2{2.0f, 0.0f}};
    cy::Vec2 out[8];

    // Clockwise input: accepting it would make the sign of `distance` mean "outward" for one caller
    // and "inward" for the next.
    const cy::Expected<cy::usize, cy::Error> wound =
        cy::geom::offset_polygon(clockwise, 4, 0.5f, cy::geom::JoinStyle::Miter, 4.0f, out, 8);
    CY_REQUIRE_FALSE(wound.has_value());
    CY_CHECK_EQ(wound.error().code, cy::ErrorCode::InvalidArgument);

    const cy::Vec2 square[] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{2.0f, 0.0f}, cy::Vec2{2.0f, 2.0f},
                               cy::Vec2{0.0f, 2.0f}};
    const cy::Expected<cy::usize, cy::Error> cramped =
        cy::geom::offset_polygon(square, 4, 0.5f, cy::geom::JoinStyle::Miter, 4.0f, out, 4);
    CY_REQUIRE_FALSE(cramped.has_value());
    CY_CHECK_EQ(cramped.error().code, cy::ErrorCode::BufferTooSmall);

    const cy::Vec2 repeated[] = {cy::Vec2{0.0f, 0.0f}, cy::Vec2{2.0f, 0.0f}, cy::Vec2{2.0f, 0.0f},
                                 cy::Vec2{0.0f, 2.0f}};
    CY_CHECK_FALSE(
        cy::geom::offset_polygon(repeated, 4, 0.5f, cy::geom::JoinStyle::Miter, 4.0f, out, 8)
            .has_value());

    // A limit below 1 would bevel a straight edge, which is not a corner at all.
    CY_CHECK_FALSE(
        cy::geom::offset_polygon(square, 4, 0.5f, cy::geom::JoinStyle::Miter, 0.5f, out, 8)
            .has_value());

    // Zero distance is the identity, and is not an error: the corner rule has nothing to do.
    const cy::Expected<cy::usize, cy::Error> unchanged =
        cy::geom::offset_polygon(square, 4, 0.0f, cy::geom::JoinStyle::Miter, 4.0f, out, 8);
    CY_REQUIRE(unchanged.has_value());
    CY_CHECK_EQ(*unchanged, cy::usize{4});
    CY_CHECK_CLOSE(polygon_area(out, 4), 4.0f, 1e-6f);
}
