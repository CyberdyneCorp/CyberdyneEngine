// Delaunay triangulation of a 2D point set. Task 3.1.5. See include/cy/core/math/geometry.h.
//
// THE ALGORITHM is Bowyer–Watson: begin with one triangle large enough to contain every point, then
// insert the points one at a time. Inserting a point deletes every triangle whose circumcircle
// contains it — together those form a star-shaped cavity — and refills the cavity by joining the
// new point to each of its boundary edges. Deleting the super-triangle's own triangles at the end
// leaves the Delaunay triangulation of the input.
//
// The defining property is negative: **no point lies inside any triangle's circumcircle**. That is
// what the test asserts, over every triangle against every point, because it is the only statement
// that distinguishes this from any other triangulation of the same points, and an implementation
// that merely produces a plausible mesh will pass anything weaker.
//
// WHY THE PREDICATE IS IN DOUBLE. `incircle` is a 3×3 determinant of coordinate differences and
// their squares. Squaring doubles the exponent range and the subtraction that follows cancels most
// of it away, so at engine scale — coordinates in the hundreds, features in the millimetres — the
// `f32` answer is dominated by rounding error near the very configurations where the answer
// matters: four nearly co-circular points. Getting one of those wrong does not tilt an edge, it
// deletes a triangle that should have survived and leaves a hole in the mesh. The inputs and the
// outputs stay `f32` because `core-math` fixes runtime precision there; only the predicate is
// widened, which is the standard and cheap half of the robustness answer. The expensive half —
// exact arithmetic with adaptive filters — is not here and would be the next step if a caller ever
// hits a case this misses.
//
// WHAT THIS IS NOT. It is not a polygon triangulator (`triangulate_polygon` is) and it is not
// constrained: there is no way to require that a particular edge appears in the result. A
// constrained Delaunay is a materially larger algorithm and nothing in this milestone needs it.

#include <cy/core/math/geometry.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace cy::geom {
namespace {

/// A point in the precision the predicates run in; see the header comment.
struct Vec2d {
    f64 x = 0.0;
    f64 y = 0.0;
};

/// A triangle of the working mesh, counter-clockwise. Indices address the input points, with the
/// three super-triangle vertices appended after them.
struct Triangle {
    u32 a = 0;
    u32 b = 0;
    u32 c = 0;
};

/// Positive when `d` lies strictly inside the circumcircle of the counter-clockwise triangle
/// (a, b, c); zero when the four are co-circular; negative when it is outside.
///
/// The lifted-paraboloid determinant: each point is mapped to (x, y, x² + y²) and the question
/// becomes which side of the plane through the lifted triangle the lifted `d` falls on. Writing it
/// as differences from `d` first is what keeps the magnitudes small — the alternative 4×4 form uses
/// the raw coordinates and loses precision to the origin's distance, which for world-space input is
/// the whole answer.
[[nodiscard]] f64 incircle(Vec2d a, Vec2d b, Vec2d c, Vec2d d) noexcept {
    const f64 adx = a.x - d.x;
    const f64 ady = a.y - d.y;
    const f64 bdx = b.x - d.x;
    const f64 bdy = b.y - d.y;
    const f64 cdx = c.x - d.x;
    const f64 cdy = c.y - d.y;

    const f64 a_lift = (adx * adx) + (ady * ady);
    const f64 b_lift = (bdx * bdx) + (bdy * bdy);
    const f64 c_lift = (cdx * cdx) + (cdy * cdy);

    return (adx * ((bdy * c_lift) - (b_lift * cdy))) - (ady * ((bdx * c_lift) - (b_lift * cdx))) +
           (a_lift * ((bdx * cdy) - (bdy * cdx)));
}

[[nodiscard]] f64 orient(Vec2d a, Vec2d b, Vec2d c) noexcept {
    return ((b.x - a.x) * (c.y - a.y)) - ((b.y - a.y) * (c.x - a.x));
}

[[nodiscard]] u64 edge_key(u32 from, u32 to) noexcept {
    return (static_cast<u64>(from) << 32) | static_cast<u64>(to);
}

/// Two input points at exactly the same coordinates. Detected by sorting rather than by comparing
/// every pair, so it costs O(n log n) instead of O(n²) on the path where there is nothing wrong.
[[nodiscard]] bool has_duplicate_point(const std::vector<Vec2d>& points) {
    std::vector<u32> order(points.size());
    for (u32 i = 0; i < static_cast<u32>(points.size()); ++i) {
        order[i] = i;
    }
    std::ranges::sort(order, [&points](u32 lhs, u32 rhs) noexcept {
        const Vec2d& a = points[lhs];
        const Vec2d& b = points[rhs];
        return a.x != b.x ? a.x < b.x : a.y < b.y;
    });
    for (usize i = 1; i < order.size(); ++i) {
        const Vec2d& previous = points[order[i - 1]];
        const Vec2d& current = points[order[i]];
        if (previous.x == current.x && previous.y == current.y) {
            return true;
        }
    }
    return false;
}

/// How far the super-triangle's vertices sit from the point set, as a multiple of its half-extent.
///
/// THIS NUMBER HAS TEETH, and it is a trade between two failures rather than a free parameter.
///
/// Too small and the triangulation loses area at its own boundary. A triangle of the true Delaunay
/// triangulation survives the scaffolding's removal only if no super-triangle vertex lies inside
/// its circumcircle — and a thin sliver against the convex hull has an enormous circumcircle. With
/// the factor of 20 that most descriptions of the algorithm use, roughly one boundary triangle in a
/// few hundred is swallowed that way and the mesh comes back with a nick out of its edge. That is
/// not a hypothetical: it is what `the triangles tile the hull of the points exactly` caught, at
/// 1492.74 against an expected 1495.09, and it is invisible to any test that does not measure area.
///
/// Too large and the predicate loses precision: the incircle determinant is a fourth-degree
/// quantity, so multiplying the coordinate range by a thousand multiplies its magnitude by 10¹²,
/// and `f64` has about sixteen digits to spend. A thousand keeps roughly four digits of headroom,
/// and the tests exercise both ends — the tiling property fails when this is smaller, and the empty
/// circumcircle property would start failing long before it is large enough to matter.
constexpr f64 kSuperScale = 1000.0;

/// The three vertices of a triangle guaranteed to contain every input point, appended to `points`.
/// Sized from the point set's own extent rather than from a fixed coordinate, so that it works at
/// any scale.
void append_super_triangle(std::vector<Vec2d>& points, usize input_count) {
    Vec2d minimum = points[0];
    Vec2d maximum = points[0];
    for (usize i = 1; i < input_count; ++i) {
        minimum.x = std::fmin(minimum.x, points[i].x);
        minimum.y = std::fmin(minimum.y, points[i].y);
        maximum.x = std::fmax(maximum.x, points[i].x);
        maximum.y = std::fmax(maximum.y, points[i].y);
    }
    const f64 centre_x = 0.5 * (minimum.x + maximum.x);
    const f64 centre_y = 0.5 * (minimum.y + maximum.y);
    const f64 radius = std::fmax(0.5 * (maximum.x - minimum.x), 0.5 * (maximum.y - minimum.y));

    points.push_back(Vec2d{centre_x - (kSuperScale * radius), centre_y - radius});
    points.push_back(Vec2d{centre_x + (kSuperScale * radius), centre_y - radius});
    points.push_back(Vec2d{centre_x, centre_y + (kSuperScale * radius)});
}

/// Insert one point: delete the triangles that see it, refill the cavity it leaves.
///
/// `doomed` and `edges` are the caller's buffers, reused across insertions.
void insert_point(const std::vector<Vec2d>& points, u32 index, std::vector<Triangle>& mesh,
                  std::vector<u32>& doomed, std::vector<u64>& edges) {
    const Vec2d p = points[index];

    doomed.clear();
    for (u32 i = 0; i < static_cast<u32>(mesh.size()); ++i) {
        const Triangle& t = mesh[i];
        if (incircle(points[t.a], points[t.b], points[t.c], p) > 0.0) {
            doomed.push_back(i);
        }
    }
    if (doomed.empty()) {
        return;
    }

    // The cavity's boundary. Every edge interior to the cavity appears twice, once in each
    // direction, because the triangles are consistently counter-clockwise; an edge with no reverse
    // is on the boundary. Sorted rather than hashed so the output order is the same on every
    // standard library — see the same choice, for the same reason, in hull3d.cpp.
    edges.clear();
    for (const u32 triangle_index : doomed) {
        const Triangle& t = mesh[triangle_index];
        edges.push_back(edge_key(t.a, t.b));
        edges.push_back(edge_key(t.b, t.c));
        edges.push_back(edge_key(t.c, t.a));
    }
    std::ranges::sort(edges);

    for (usize i = doomed.size(); i-- > 0;) {
        mesh[doomed[i]] = mesh.back();
        mesh.pop_back();
    }

    for (const u64 key : edges) {
        const u32 from = static_cast<u32>(key >> 32);
        const u32 to = static_cast<u32>(key & 0xFFFFFFFFu);
        if (std::ranges::binary_search(edges, edge_key(to, from))) {
            continue;
        }
        // The boundary edge runs counter-clockwise around the cavity, so (from, to, p) is
        // counter-clockwise too and the invariant every predicate here assumes is maintained.
        mesh.push_back(Triangle{from, to, index});
    }
}

}  // namespace

Expected<usize, Error> triangulate_delaunay(const Vec2* points, usize count, u32* out_indices,
                                            usize out_capacity) noexcept {
    if (points == nullptr || out_indices == nullptr) {
        return cy::fail(ErrorCode::InvalidArgument, "triangulate_delaunay(): a null array");
    }
    if (count < 3) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "triangulate_delaunay(): a triangulation needs at least three points");
    }
    if (out_capacity < 6 * count) {
        return cy::fail(ErrorCode::BufferTooSmall,
                        "triangulate_delaunay(): out_indices needs room for 6 * count");
    }

    std::vector<Vec2d> working;
    working.reserve(count + 3);
    for (usize i = 0; i < count; ++i) {
        working.push_back(Vec2d{static_cast<f64>(points[i].x), static_cast<f64>(points[i].y)});
    }
    if (has_duplicate_point(working)) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "triangulate_delaunay(): two points are at the same coordinates — weld "
                        "them first, a duplicate has no Delaunay answer");
    }
    append_super_triangle(working, count);

    std::vector<Triangle> mesh;
    mesh.reserve((2 * count) + 8);
    mesh.push_back(Triangle{static_cast<u32>(count), static_cast<u32>(count + 1),
                            static_cast<u32>(count + 2)});

    std::vector<u32> doomed;
    std::vector<u64> edges;
    for (usize i = 0; i < count; ++i) {
        insert_point(working, static_cast<u32>(i), mesh, doomed, edges);
    }

    usize written = 0;
    for (const Triangle& t : mesh) {
        // Anything still touching the super-triangle is scaffolding, not triangulation.
        if (t.a >= count || t.b >= count || t.c >= count) {
            continue;
        }
        // A zero-area triangle can survive when several points are collinear on the hull; it
        // carries no area and every consumer would have to filter it, so it is filtered here.
        if (orient(working[t.a], working[t.b], working[t.c]) <= 0.0) {
            continue;
        }
        if (written + 3 > out_capacity) {
            return cy::fail(ErrorCode::Internal,
                            "triangulate_delaunay(): produced more triangles than 2n - 5");
        }
        out_indices[written++] = t.a;
        out_indices[written++] = t.b;
        out_indices[written++] = t.c;
    }

    if (written == 0) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "triangulate_delaunay(): the points are collinear and have no "
                        "triangulation");
    }
    return written;
}

}  // namespace cy::geom
