// Geometry utilities. Task 3.1.5.
//
// `core-math` — "Geometry utilities", including its "Ray/triangle for picking" scenario: a ray
// query followed by ray/triangle tests returns the nearest hit **with barycentric coordinates**,
// because that is what the editor interpolates a UV or a vertex colour with. The barycentric
// coordinates are therefore asserted against a known point rather than merely checked for being in
// range.
//
// The routines that produce a variable number of results are also tested for the buffer-too-small
// path. A routine that silently wrote past the end of a caller's array would pass every other test
// in this file.

#include <cy/core/math/math.h>

#include <cy/test/test.h>

#include "approx.h"

#include <cmath>
#include <vector>

CY_TEST_CASE("Ray casts: AABB, sphere and plane") {
    const cy::Aabb box{cy::Vec3{-1.0f, -1.0f, -1.0f}, cy::Vec3{1.0f, 1.0f, 1.0f}};
    cy::f32 enter = 0.0f;
    cy::f32 exit = 0.0f;

    const cy::Ray straight{cy::Vec3{-5.0f, 0.0f, 0.0f}, cy::kAxisX};
    CY_REQUIRE(cy::geom::ray_aabb(straight, box, 100.0f, enter, exit));
    CY_CHECK_CLOSE(enter, 4.0f, 1e-6f);
    CY_CHECK_CLOSE(exit, 6.0f, 1e-6f);

    // An origin inside the box enters at zero.
    const cy::Ray inside{cy::Vec3{}, cy::kAxisX};
    CY_REQUIRE(cy::geom::ray_aabb(inside, box, 100.0f, enter, exit));
    CY_CHECK_EQ(enter, 0.0f);
    CY_CHECK_CLOSE(exit, 1.0f, 1e-6f);

    // Parallel to a slab and outside it: the axis-aligned degenerate case that a naive
    // divide-by-direction turns into a NaN.
    const cy::Ray parallel{cy::Vec3{0.0f, 5.0f, 0.0f}, cy::kAxisX};
    CY_CHECK_FALSE(cy::geom::ray_aabb(parallel, box, 100.0f, enter, exit));

    // Pointing away, and running out of distance.
    CY_CHECK_FALSE(cy::geom::ray_aabb(cy::Ray{cy::Vec3{-5.0f, 0.0f, 0.0f}, -cy::kAxisX}, box,
                                      100.0f, enter, exit));
    CY_CHECK_FALSE(cy::geom::ray_aabb(straight, box, 3.0f, enter, exit));

    cy::f32 t = 0.0f;
    const cy::Sphere sphere{cy::Vec3{0.0f, 0.0f, 0.0f}, 2.0f};
    CY_REQUIRE(
        cy::geom::ray_sphere(cy::Ray{cy::Vec3{0.0f, 0.0f, 10.0f}, -cy::kAxisZ}, sphere, 100.0f, t));
    CY_CHECK_CLOSE(t, 8.0f, 1e-5f);
    // From inside, the reported hit is the exit point.
    CY_REQUIRE(cy::geom::ray_sphere(cy::Ray{cy::Vec3{}, cy::kAxisX}, sphere, 100.0f, t));
    CY_CHECK_CLOSE(t, 2.0f, 1e-5f);
    CY_CHECK_FALSE(
        cy::geom::ray_sphere(cy::Ray{cy::Vec3{0.0f, 10.0f, 0.0f}, cy::kAxisX}, sphere, 100.0f, t));

    const cy::Plane ground = cy::Plane::from_point_normal(cy::Vec3{}, cy::kAxisUp);
    CY_REQUIRE(
        cy::geom::ray_plane(cy::Ray{cy::Vec3{0.0f, 7.0f, 0.0f}, -cy::kAxisY}, ground, 100.0f, t));
    CY_CHECK_CLOSE(t, 7.0f, 1e-6f);
    // A ray parallel to the plane never hits, including one lying in it.
    CY_CHECK_FALSE(
        cy::geom::ray_plane(cy::Ray{cy::Vec3{0.0f, 1.0f, 0.0f}, cy::kAxisX}, ground, 100.0f, t));
    CY_CHECK_FALSE(cy::geom::ray_plane(cy::Ray{cy::Vec3{}, cy::kAxisX}, ground, 100.0f, t));
}

CY_TEST_CASE("Ray/triangle: the hit carries usable barycentric coordinates") {
    // A triangle in the z = 0 plane, wound counter-clockwise seen from +Z.
    const cy::Vec3 v0{0.0f, 0.0f, 0.0f};
    const cy::Vec3 v1{1.0f, 0.0f, 0.0f};
    const cy::Vec3 v2{0.0f, 1.0f, 0.0f};

    cy::geom::TriangleHit hit;
    // Aimed at the centroid: the barycentric weights are all a third, which is the value that
    // catches a u/v swap as well as a scale error.
    const cy::Ray at_centroid{cy::Vec3{1.0f / 3.0f, 1.0f / 3.0f, 5.0f}, -cy::kAxisZ};
    CY_REQUIRE(cy::geom::ray_triangle(at_centroid, v0, v1, v2, 100.0f, false, hit));
    CY_CHECK_CLOSE(hit.t, 5.0f, 1e-5f);
    CY_CHECK_CLOSE(hit.u, 1.0f / 3.0f, 1e-4f);
    CY_CHECK_CLOSE(hit.v, 1.0f / 3.0f, 1e-4f);
    CY_CHECK_CLOSE(1.0f - hit.u - hit.v, 1.0f / 3.0f, 1e-4f);

    // The hit position reconstructed from the weights is the hit position along the ray, which is
    // the property the picking path actually uses.
    const cy::Vec3 from_barycentrics = v0 * (1.0f - hit.u - hit.v) + v1 * hit.u + v2 * hit.v;
    CY_CHECK(nearly_equal(from_barycentrics, at_centroid.at(hit.t), 1e-4f));

    // Near v1, where u dominates. Aimed a hair inside rather than exactly at the vertex: a hit
    // exactly on an edge is a coin toss in floating point, and this test is about the coordinates
    // rather than about the boundary policy.
    CY_REQUIRE(cy::geom::ray_triangle(cy::Ray{cy::Vec3{0.9f, 0.05f, 5.0f}, -cy::kAxisZ}, v0, v1, v2,
                                      100.0f, false, hit));
    CY_CHECK_CLOSE(hit.u, 0.9f, 1e-4f);
    CY_CHECK_CLOSE(hit.v, 0.05f, 1e-4f);

    // Outside the triangle, and behind the ray.
    CY_CHECK_FALSE(cy::geom::ray_triangle(cy::Ray{cy::Vec3{1.0f, 1.0f, 5.0f}, -cy::kAxisZ}, v0, v1,
                                          v2, 100.0f, false, hit));
    CY_CHECK_FALSE(cy::geom::ray_triangle(cy::Ray{cy::Vec3{0.2f, 0.2f, 5.0f}, cy::kAxisZ}, v0, v1,
                                          v2, 100.0f, false, hit));

    // Back-face culling. Seen from -Z the winding is clockwise, so this is the back face: reported
    // when two-sided, rejected when culling.
    const cy::Ray from_behind{cy::Vec3{0.2f, 0.2f, -5.0f}, cy::kAxisZ};
    CY_REQUIRE(cy::geom::ray_triangle(from_behind, v0, v1, v2, 100.0f, false, hit));
    CY_CHECK(hit.back_face);
    CY_CHECK_FALSE(cy::geom::ray_triangle(from_behind, v0, v1, v2, 100.0f, true, hit));
}

CY_TEST_CASE("Closest points: segments, including the degenerate and parallel cases") {
    cy::f32 s = 0.0f;
    cy::f32 t = 0.0f;
    cy::Vec3 pa;
    cy::Vec3 pb;

    // Two crossing segments, offset in y: the closest points are directly above each other.
    cy::geom::closest_points_segment_segment(
        cy::Vec3{-1.0f, 0.0f, 0.0f}, cy::Vec3{1.0f, 0.0f, 0.0f}, cy::Vec3{0.0f, 3.0f, -1.0f},
        cy::Vec3{0.0f, 3.0f, 1.0f}, s, t, pa, pb);
    CY_CHECK_CLOSE(s, 0.5f, 1e-5f);
    CY_CHECK_CLOSE(t, 0.5f, 1e-5f);
    CY_CHECK(nearly_equal(pa, cy::Vec3{0.0f, 0.0f, 0.0f}, 1e-5f));
    CY_CHECK(nearly_equal(pb, cy::Vec3{0.0f, 3.0f, 0.0f}, 1e-5f));

    // Parallel: any pair at the same offset is closest, and the implementation must pick one rather
    // than divide by a zero denominator.
    cy::geom::closest_points_segment_segment(cy::Vec3{0.0f, 0.0f, 0.0f}, cy::Vec3{1.0f, 0.0f, 0.0f},
                                             cy::Vec3{0.0f, 1.0f, 0.0f}, cy::Vec3{1.0f, 1.0f, 0.0f},
                                             s, t, pa, pb);
    CY_CHECK_CLOSE(distance(pa, pb), 1.0f, 1e-5f);

    // Both degenerate: two points.
    cy::geom::closest_points_segment_segment(cy::Vec3{2.0f, 0.0f, 0.0f}, cy::Vec3{2.0f, 0.0f, 0.0f},
                                             cy::Vec3{0.0f, 0.0f, 0.0f}, cy::Vec3{0.0f, 0.0f, 0.0f},
                                             s, t, pa, pb);
    CY_CHECK(nearly_equal(pa, cy::Vec3{2.0f, 0.0f, 0.0f}, 1e-6f));
    CY_CHECK(nearly_equal(pb, cy::Vec3{}, 1e-6f));

    CY_CHECK(nearly_equal(cy::geom::closest_point_on_segment(cy::Vec3{0.5f, 5.0f, 0.0f}, cy::Vec3{},
                                                             cy::Vec3{1.0f, 0.0f, 0.0f}),
                          cy::Vec3{0.5f, 0.0f, 0.0f}, 1e-6f));
    // Clamped to the ends rather than extrapolated.
    CY_CHECK(nearly_equal(cy::geom::closest_point_on_segment(
                              cy::Vec3{-9.0f, 0.0f, 0.0f}, cy::Vec3{}, cy::Vec3{1.0f, 0.0f, 0.0f}),
                          cy::Vec3{}, 1e-6f));
}

CY_TEST_CASE("Closest point on a triangle covers the face, edge and vertex regions") {
    const cy::Vec3 v0{0.0f, 0.0f, 0.0f};
    const cy::Vec3 v1{4.0f, 0.0f, 0.0f};
    const cy::Vec3 v2{0.0f, 4.0f, 0.0f};

    // Above the face.
    CY_CHECK(
        nearly_equal(cy::geom::closest_point_on_triangle(cy::Vec3{1.0f, 1.0f, 9.0f}, v0, v1, v2),
                     cy::Vec3{1.0f, 1.0f, 0.0f}, 1e-5f));
    // Past a vertex.
    CY_CHECK(nearly_equal(
        cy::geom::closest_point_on_triangle(cy::Vec3{-3.0f, -3.0f, 0.0f}, v0, v1, v2), v0, 1e-5f));
    // Beyond an edge.
    CY_CHECK(
        nearly_equal(cy::geom::closest_point_on_triangle(cy::Vec3{2.0f, -5.0f, 0.0f}, v0, v1, v2),
                     cy::Vec3{2.0f, 0.0f, 0.0f}, 1e-5f));
}

CY_TEST_CASE("Polygons: containment, area and the convex hull") {
    const cy::Vec2 square[4] = {{0.0f, 0.0f}, {4.0f, 0.0f}, {4.0f, 4.0f}, {0.0f, 4.0f}};
    CY_CHECK(cy::geom::point_in_polygon(cy::Vec2{2.0f, 2.0f}, square, 4));
    CY_CHECK_FALSE(cy::geom::point_in_polygon(cy::Vec2{5.0f, 2.0f}, square, 4));
    CY_CHECK_FALSE(cy::geom::point_in_polygon(cy::Vec2{2.0f, -0.5f}, square, 4));

    // A concave polygon, where a convex-only test would answer wrongly: an L shape, with the point
    // inside the bounding box but outside the polygon.
    const cy::Vec2 ell[6] = {{0.0f, 0.0f}, {4.0f, 0.0f}, {4.0f, 1.0f},
                             {1.0f, 1.0f}, {1.0f, 4.0f}, {0.0f, 4.0f}};
    CY_CHECK(cy::geom::point_in_polygon(cy::Vec2{0.5f, 3.0f}, ell, 6));
    CY_CHECK_FALSE(cy::geom::point_in_polygon(cy::Vec2{3.0f, 3.0f}, ell, 6));

    // Twice the signed area: positive for counter-clockwise.
    CY_CHECK_CLOSE(cy::geom::polygon_signed_area2(square, 4), 32.0f, 1e-5f);
    const cy::Vec2 clockwise[4] = {{0.0f, 0.0f}, {0.0f, 4.0f}, {4.0f, 4.0f}, {4.0f, 0.0f}};
    CY_CHECK_CLOSE(cy::geom::polygon_signed_area2(clockwise, 4), -32.0f, 1e-5f);

    // The hull of a square plus interior and edge-collinear points is the four corners.
    const cy::Vec2 points[8] = {{0.0f, 0.0f}, {4.0f, 0.0f}, {4.0f, 4.0f}, {0.0f, 4.0f},
                                {2.0f, 2.0f}, {1.0f, 1.0f}, {2.0f, 0.0f}, {3.5f, 3.5f}};
    cy::u32 hull[9];
    cy::u32 scratch[8];
    const cy::Expected<cy::usize, cy::Error> hull_size =
        cy::geom::convex_hull_2d(points, 8, hull, 9, scratch);
    CY_REQUIRE(hull_size.has_value());
    CY_CHECK_EQ(*hull_size, 4u);
    for (cy::usize i = 0; i < *hull_size; ++i) {
        // Computed into a named bool: doctest decomposes a comparison and refuses a compound
        // expression, and the refusal is a static_assert rather than a warning.
        const cy::Vec2 p = points[hull[i]];
        const bool is_corner = (p.x == 0.0f || p.x == 4.0f) && (p.y == 0.0f || p.y == 4.0f);
        CY_CHECK(is_corner);
    }

    // A buffer that cannot hold the answer is reported, not written past.
    CY_CHECK_FALSE(cy::geom::convex_hull_2d(points, 8, hull, 2, scratch).has_value());
}

CY_TEST_CASE("Triangulation: ear clipping covers a concave polygon exactly once") {
    // The L shape again: a convex fan from vertex 0 would put a triangle outside the polygon, so
    // this is the case that distinguishes a real ear-clipper from a fan.
    const cy::Vec2 ell[6] = {{0.0f, 0.0f}, {4.0f, 0.0f}, {4.0f, 1.0f},
                             {1.0f, 1.0f}, {1.0f, 4.0f}, {0.0f, 4.0f}};
    cy::u32 indices[12];
    const cy::Expected<cy::usize, cy::Error> written =
        cy::geom::triangulate_polygon(ell, 6, indices, 12);
    CY_REQUIRE(written.has_value());
    CY_CHECK_EQ(*written, 12u);

    // The triangles tile the polygon: their total area equals the polygon's, and every one of them
    // is wound counter-clockwise (positive area), so none is inverted or outside.
    cy::f32 total = 0.0f;
    for (cy::usize i = 0; i < *written; i += 3) {
        const cy::Vec2 a = ell[indices[i]];
        const cy::Vec2 b = ell[indices[i + 1]];
        const cy::Vec2 c = ell[indices[i + 2]];
        const cy::f32 area2 = cross(b - a, c - a);
        CY_REQUIRE(area2 > 0.0f);
        total += area2 * 0.5f;
    }
    CY_CHECK_CLOSE(total, 0.5f * cy::geom::polygon_signed_area2(ell, 6), 1e-4f);

    CY_CHECK_FALSE(cy::geom::triangulate_polygon(ell, 2, indices, 12).has_value());
    CY_CHECK_FALSE(cy::geom::triangulate_polygon(ell, 6, indices, 5).has_value());
}

CY_TEST_CASE("Clipping: a polygon cut by a plane keeps the normal's side") {
    // A unit square in the z = 0 plane, clipped by x >= 0.
    const cy::Vec3 square[4] = {
        {-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {-1.0f, 1.0f, 0.0f}};
    const cy::Plane keep_positive_x = cy::Plane::from_point_normal(cy::Vec3{}, cy::kAxisX);

    cy::Vec3 clipped[5];
    const cy::Expected<cy::usize, cy::Error> count =
        cy::geom::clip_polygon_by_plane(square, 4, keep_positive_x, clipped, 5);
    CY_REQUIRE(count.has_value());
    CY_CHECK_EQ(*count, 4u);
    for (cy::usize i = 0; i < *count; ++i) {
        CY_CHECK(clipped[i].x >= -1e-6f);
    }

    // Clipping by a plane that everything is behind removes the polygon entirely.
    const cy::Plane keep_far =
        cy::Plane::from_point_normal(cy::Vec3{100.0f, 0.0f, 0.0f}, cy::kAxisX);
    const cy::Expected<cy::usize, cy::Error> nothing =
        cy::geom::clip_polygon_by_plane(square, 4, keep_far, clipped, 5);
    CY_REQUIRE(nothing.has_value());
    CY_CHECK_EQ(*nothing, 0u);

    // A plane set: the square clipped to its own top-right quadrant.
    const cy::Plane planes[2] = {keep_positive_x,
                                 cy::Plane::from_point_normal(cy::Vec3{}, cy::kAxisY)};
    cy::Vec3 result[6];
    cy::Vec3 scratch[6];
    const cy::Expected<cy::usize, cy::Error> quadrant =
        cy::geom::clip_polygon_by_planes(square, 4, planes, 2, result, scratch, 6);
    CY_REQUIRE(quadrant.has_value());
    CY_CHECK_EQ(*quadrant, 4u);
    for (cy::usize i = 0; i < *quadrant; ++i) {
        CY_CHECK(result[i].x >= -1e-6f);
        CY_CHECK(result[i].y >= -1e-6f);
    }
}

CY_TEST_CASE("AtlasPacker: rectangles are placed without overlapping") {
    cy::geom::AtlasPacker packer(cy::IVec2{256, 256});
    CY_CHECK_EQ(packer.occupancy(), 0.0f);

    cy::Random random(2024ull);
    std::vector<cy::IRect> placed;
    bool all_in_bounds = true;
    bool all_sized_right = true;
    for (int i = 0; i < 120; ++i) {
        const cy::IVec2 size{random.next_int_in(8, 40), random.next_int_in(8, 40)};
        const cy::Expected<cy::IRect, cy::Error> slot = packer.pack(size);
        if (!slot) {
            // Running out of room is an ordinary answer, not a failure.
            CY_CHECK(slot.error().code == cy::ErrorCode::OutOfRange);
            break;
        }
        all_sized_right = all_sized_right && slot->size == size;
        all_in_bounds = all_in_bounds && slot->position.x >= 0 && slot->position.y >= 0 &&
                        slot->max().x <= 256 && slot->max().y <= 256;
        placed.push_back(*slot);
    }
    CY_CHECK(all_sized_right);
    CY_CHECK(all_in_bounds);
    CY_CHECK(placed.size() > 20);

    // The property that matters: no two placements overlap. A packer that gets this wrong produces
    // an atlas where one glyph bleeds into another.
    //
    // The whole quadratic sweep is folded into one assertion rather than one per pair. Ten thousand
    // doctest assertions cost far more than the ten thousand rectangle tests they wrap, and this
    // test's budget is a millisecond.
    bool any_overlap = false;
    for (cy::usize i = 0; i < placed.size() && !any_overlap; ++i) {
        for (cy::usize j = i + 1; j < placed.size(); ++j) {
            if (placed[i].intersects(placed[j])) {
                any_overlap = true;
                break;
            }
        }
    }
    CY_CHECK_FALSE(any_overlap);

    CY_CHECK(packer.occupancy() > 0.0f);
    CY_CHECK(packer.occupancy() <= 1.0f);

    // A rectangle larger than the atlas never fits, and a degenerate one is rejected outright.
    CY_CHECK_FALSE(packer.pack(cy::IVec2{512, 512}).has_value());
    CY_CHECK_FALSE(packer.pack(cy::IVec2{0, 10}).has_value());
}

CY_TEST_CASE("weld_vertices: coincident positions merge and the remap is consistent") {
    const cy::Vec3 positions[6] = {
        {0.0f, 0.0f, 0.0f},  {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},  {0.0f, 0.0f, 1e-7f},  // within tolerance of the first
        {1.0f, 0.0f, 1e-7f},                       // within tolerance of the second
        {5.0f, 5.0f, 5.0f},
    };
    cy::u32 remap[6];
    cy::Vec3 unique[6];
    const cy::Expected<cy::usize, cy::Error> count =
        cy::geom::weld_vertices(positions, 6, 1e-4f, remap, unique);
    CY_REQUIRE(count.has_value());
    CY_CHECK_EQ(*count, 4u);
    CY_CHECK_EQ(remap[0], remap[3]);
    CY_CHECK_EQ(remap[1], remap[4]);
    CY_CHECK_NE(remap[0], remap[1]);
    CY_CHECK_NE(remap[0], remap[5]);
    for (cy::usize i = 0; i < 6; ++i) {
        CY_REQUIRE(remap[i] < *count);
        CY_CHECK(distance(unique[remap[i]], positions[i]) <= 1e-4f);
    }

    // A tolerance that merges nothing leaves every vertex distinct.
    const cy::Expected<cy::usize, cy::Error> strict =
        cy::geom::weld_vertices(positions, 6, 1e-8f, remap, unique);
    CY_REQUIRE(strict.has_value());
    CY_CHECK_EQ(*strict, 6u);
    CY_CHECK_FALSE(cy::geom::weld_vertices(positions, 6, 0.0f, remap, unique).has_value());
}

CY_TEST_CASE("generate_tangents: tangents follow the UV gradient and carry handedness") {
    // A quad in the XY plane with UVs running along +X and +Y, so the tangent must be +X.
    const cy::Vec3 positions[4] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    const cy::Vec3 normals[4] = {cy::kAxisZ, cy::kAxisZ, cy::kAxisZ, cy::kAxisZ};
    const cy::Vec2 uvs[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    const cy::u32 indices[6] = {0, 1, 2, 0, 2, 3};

    cy::Vec4 tangents[4];
    CY_REQUIRE(
        cy::geom::generate_tangents(positions, normals, uvs, 4, indices, 6, tangents).has_value());
    for (const cy::Vec4& tangent : tangents) {
        CY_CHECK(nearly_equal(tangent.xyz(), cy::kAxisX, 1e-4f));
        CY_CHECK_CLOSE(std::fabs(tangent.w), 1.0f, 1e-6f);
        // Orthonormal against the normal, which is what the shader's frame reconstruction assumes.
        CY_CHECK(cy::math::nearly_zero(dot(tangent.xyz(), cy::kAxisZ), 1e-5f));
    }

    // Mirrored UVs flip the handedness, which is the whole reason the fourth component exists.
    const cy::Vec2 mirrored[4] = {{1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    CY_REQUIRE(cy::geom::generate_tangents(positions, normals, mirrored, 4, indices, 6, tangents)
                   .has_value());
    CY_CHECK(nearly_equal(tangents[0].xyz(), -cy::kAxisX, 1e-4f));

    // Degenerate UVs give an arbitrary but finite perpendicular tangent rather than a zero vector
    // that would become a NaN in the shader.
    const cy::Vec2 collapsed[4] = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
    CY_REQUIRE(cy::geom::generate_tangents(positions, normals, collapsed, 4, indices, 6, tangents)
                   .has_value());
    for (const cy::Vec4& tangent : tangents) {
        CY_CHECK_CLOSE(length(tangent.xyz()), 1.0f, 1e-4f);
        CY_CHECK(cy::math::nearly_zero(dot(tangent.xyz(), cy::kAxisZ), 1e-5f));
    }

    // An index outside the vertex array is reported rather than read.
    const cy::u32 bad_indices[3] = {0, 1, 99};
    CY_CHECK_FALSE(cy::geom::generate_tangents(positions, normals, uvs, 4, bad_indices, 3, tangents)
                       .has_value());
}
