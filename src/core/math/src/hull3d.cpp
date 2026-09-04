// The 3D convex hull. Task 3.1.5. See include/cy/core/math/geometry.h.
//
// THE ALGORITHM is incremental face-stitching — quickhull's construction without its recursive
// partition. Seed a tetrahedron from four extreme points, then take each remaining point in turn:
// delete every face it can see, and stitch new faces from the point to the horizon those deletions
// left behind. A point that sees no face is already inside and costs one pass over the faces.
//
// It is O(n · f) rather than quickhull's expected O(n log n), and it is what belongs here: the
// clouds this engine hulls are a mesh's vertices for a collision proxy or a light's influence
// volume — thousands of points at cook time, not millions at run time — and the recursive
// partition's bookkeeping (conflict lists per face, points reassigned as faces die) is a
// substantial amount of state to get wrong for a constant factor nobody in this milestone can
// measure. If a caller ever hulls a million points, the shape of this file is what changes, not its
// interface.
//
// WHY THE PREDICATES ARE IN DOUBLE. Whether a point is outside a face is a signed distance: a
// difference of products of coordinates that are nearly equal. In `f32` at engine scale — a
// 100-metre mesh with millimetre features — that difference loses most of its significant bits, and
// the failure mode is not a slightly wrong hull. It is a face wrongly judged visible from a point
// on its far side, whose deletion opens a hole that the horizon walk then stitches shut across the
// hull's interior. Every distance here is therefore computed in `f64` from `f32` inputs, which is
// exact for the subtraction and costs nothing measurable at these sizes.
//
// THE TOLERANCE IS RELATIVE, and that is the one number a caller has to think about. It scales by
// the cloud's own extent, so the same value works for a 1-metre prop and a 1-kilometre landscape,
// and a point within it of the current hull's surface is treated as inside. That is what stops a
// nearly-flat cluster of points from generating a spray of sliver faces, each of which then makes
// the next visibility test worse.

#include <cy/core/math/geometry.h>

#include <cy/core/base/assert.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace cy::geom {
namespace {

/// A point in the precision the predicates run in. Not a `Vec3`: `Vec3` is the storage type and is
/// deliberately 32-bit, and a `Vec3d` sitting next to it in the same file is a reminder that the
/// conversion is at the boundary and happens once.
struct Vec3d {
    f64 x = 0.0;
    f64 y = 0.0;
    f64 z = 0.0;
};

[[nodiscard]] Vec3d to_double(Vec3 v) noexcept {
    return Vec3d{static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)};
}

[[nodiscard]] Vec3d sub(Vec3d a, Vec3d b) noexcept {
    return Vec3d{a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] Vec3d cross3(Vec3d a, Vec3d b) noexcept {
    return Vec3d{(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

[[nodiscard]] f64 dot3(Vec3d a, Vec3d b) noexcept {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

[[nodiscard]] f64 length3(Vec3d a) noexcept {
    return std::sqrt(dot3(a, a));
}

/// One hull face: three vertex indices, counter-clockwise seen from outside, and the plane they
/// span with a **unit** normal — unit so that `distance()` is a length in metres and can be
/// compared against the tolerance, rather than a value scaled by the triangle's area.
struct Face {
    u32 a = 0;
    u32 b = 0;
    u32 c = 0;
    Vec3d normal{};
    f64 offset = 0.0;

    [[nodiscard]] f64 distance(Vec3d p) const noexcept { return dot3(normal, p) + offset; }
};

/// The face spanned by three points, or `false` when they are collinear and span nothing.
[[nodiscard]] bool make_face(const Vec3d* points, u32 a, u32 b, u32 c, Face& out) noexcept {
    const Vec3d normal = cross3(sub(points[b], points[a]), sub(points[c], points[a]));
    const f64 magnitude = length3(normal);
    if (magnitude <= 0.0) {
        return false;
    }
    out.a = a;
    out.b = b;
    out.c = c;
    out.normal = Vec3d{normal.x / magnitude, normal.y / magnitude, normal.z / magnitude};
    out.offset = -dot3(out.normal, points[a]);
    return true;
}

/// A directed edge packed into one integer, so that a list of them can be sorted and searched. The
/// horizon walk asks "is the *opposite* direction of this edge also on a face I am deleting?", and
/// packing the direction into the key is what makes that one lookup rather than a scan.
[[nodiscard]] u64 edge_key(u32 from, u32 to) noexcept {
    return (static_cast<u64>(from) << 32) | static_cast<u64>(to);
}

/// The six axis extremes, which is where a maximally distant pair is always found among.
void axis_extremes(const Vec3d* points, usize count, u32 (&out)[6]) noexcept {
    for (u32& index : out) {
        index = 0;
    }
    for (usize i = 1; i < count; ++i) {
        const Vec3d p = points[i];
        if (p.x < points[out[0]].x) {
            out[0] = static_cast<u32>(i);
        }
        if (p.x > points[out[1]].x) {
            out[1] = static_cast<u32>(i);
        }
        if (p.y < points[out[2]].y) {
            out[2] = static_cast<u32>(i);
        }
        if (p.y > points[out[3]].y) {
            out[3] = static_cast<u32>(i);
        }
        if (p.z < points[out[4]].z) {
            out[4] = static_cast<u32>(i);
        }
        if (p.z > points[out[5]].z) {
            out[5] = static_cast<u32>(i);
        }
    }
}

/// The tolerance in metres: the caller's relative figure against the cloud's largest extent, so
/// that it means the same thing for a 1-metre prop and a 1-kilometre landscape. A cloud with no
/// extent at all comes back with a zero epsilon and is caught by the seed search, which is the one
/// place that can tell "no extent" from "no volume".
[[nodiscard]] f64 cloud_epsilon(const std::vector<Vec3d>& points, f32 relative_tolerance) noexcept {
    Vec3d minimum = points[0];
    Vec3d maximum = points[0];
    for (const Vec3d& p : points) {
        minimum =
            Vec3d{std::fmin(p.x, minimum.x), std::fmin(p.y, minimum.y), std::fmin(p.z, minimum.z)};
        maximum =
            Vec3d{std::fmax(p.x, maximum.x), std::fmax(p.y, maximum.y), std::fmax(p.z, maximum.z)};
    }
    const Vec3d extent = sub(maximum, minimum);
    const f64 largest = std::fmax(extent.x, std::fmax(extent.y, extent.z));
    return static_cast<f64>(relative_tolerance) * largest;
}

/// Everything the caller can get wrong before a point is looked at.
[[nodiscard]] Expected<void, Error> check_hull_arguments(const Vec3* points, usize count,
                                                         const u32* out_indices,
                                                         usize out_capacity) noexcept {
    if (points == nullptr || out_indices == nullptr) {
        return cy::fail(ErrorCode::InvalidArgument, "convex_hull_3d(): a null array");
    }
    if (count < 4) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "convex_hull_3d(): a hull with volume needs at least four points");
    }
    if (out_capacity < 6 * count) {
        return cy::fail(ErrorCode::BufferTooSmall,
                        "convex_hull_3d(): out_indices needs room for 6 * count");
    }
    return {};
}

/// Four points that span a volume, or `false` when the cloud is flat within `epsilon`.
///
/// Each is the furthest point from what the previous ones span: the longest of the fifteen pairs of
/// axis extremes, then the point furthest from that line, then the point furthest from that plane.
/// Seeding from extremes rather than from the first four points is what keeps the initial
/// tetrahedron large, and a large seed is what keeps the incremental step deleting many faces at
/// once instead of one.
[[nodiscard]] bool find_seed_tetrahedron(const Vec3d* points, usize count, f64 epsilon,
                                         u32 (&seed)[4]) noexcept {
    u32 extremes[6];
    axis_extremes(points, count, extremes);

    f64 best = -1.0;
    for (usize i = 0; i < 6; ++i) {
        for (usize j = i + 1; j < 6; ++j) {
            const f64 distance = length3(sub(points[extremes[i]], points[extremes[j]]));
            if (distance > best) {
                best = distance;
                seed[0] = extremes[i];
                seed[1] = extremes[j];
            }
        }
    }
    if (best <= epsilon) {
        return false;  // Every point is the same point.
    }

    const Vec3d line = sub(points[seed[1]], points[seed[0]]);
    const f64 line_length = length3(line);
    best = -1.0;
    for (usize i = 0; i < count; ++i) {
        // |(p - a) × d| / |d| is the perpendicular distance to the line, and the cross product is
        // the same quantity the face plane will need, so it is not a second formulation.
        const f64 distance = length3(cross3(sub(points[i], points[seed[0]]), line)) / line_length;
        if (distance > best) {
            best = distance;
            seed[2] = static_cast<u32>(i);
        }
    }
    if (best <= epsilon) {
        return false;  // Collinear.
    }

    Face base;
    if (!make_face(points, seed[0], seed[1], seed[2], base)) {
        return false;
    }
    best = -1.0;
    for (usize i = 0; i < count; ++i) {
        const f64 distance = std::fabs(base.distance(points[i]));
        if (distance > best) {
            best = distance;
            seed[3] = static_cast<u32>(i);
        }
    }
    return best > epsilon;  // Coplanar when it is not.
}

/// The four faces of the seed tetrahedron, each already facing outward.
///
/// The base is oriented so the fourth point is behind it, and each side face then reuses one base
/// edge **reversed**: an edge traversed one way on the face inside the hull is traversed the other
/// way on the face outside it, which is the invariant the horizon stitch below relies on too.
void build_seed_faces(const Vec3d* points, const u32 (&seed)[4], std::vector<Face>& faces) {
    Face base;
    const bool spanned = make_face(points, seed[0], seed[1], seed[2], base);
    CY_ASSERT_MSG(spanned, "convex_hull_3d(): the seed triangle was checked to span a plane");
    (void)spanned;

    u32 a = seed[0];
    u32 b = seed[1];
    const u32 c = seed[2];
    const u32 d = seed[3];
    if (base.distance(points[d]) > 0.0) {
        // The apex is in front of the base, so the base is facing inward: swapping two vertices
        // reverses its winding and its normal.
        const u32 swapped = a;
        a = b;
        b = swapped;
    }

    Face face;
    const u32 triangles[4][3] = {{a, b, c}, {b, a, d}, {c, b, d}, {a, c, d}};
    for (const auto& triangle : triangles) {
        if (make_face(points, triangle[0], triangle[1], triangle[2], face)) {
            faces.push_back(face);
        }
    }
}

/// The scratch three buffers deep that `add_point` needs. Owned by the point loop and reused, so
/// that adding a point allocates nothing after the first few: a hull of ten thousand points
/// otherwise pays three vector allocations per point for buffers whose size barely changes.
struct HullScratch {
    std::vector<u32> visible;
    std::vector<u64> edges;
    std::vector<Face> additions;
};

/// Add one point to the hull. Returns false only when the point was inside it, or when the stitch
/// would have produced nothing — either way the hull is left as it was.
bool add_point(const Vec3d* points, u32 index, f64 epsilon, std::vector<Face>& faces,
               HullScratch& scratch) {
    const Vec3d p = points[index];

    scratch.visible.clear();
    for (u32 i = 0; i < static_cast<u32>(faces.size()); ++i) {
        if (faces[i].distance(p) > epsilon) {
            scratch.visible.push_back(i);
        }
    }
    if (scratch.visible.empty()) {
        return false;  // Inside the hull, or on it within the tolerance.
    }

    // Every directed edge of every doomed face. An edge whose reverse is also here joins two doomed
    // faces and vanishes with them; an edge whose reverse is not is on the horizon.
    //
    // A SORTED VECTOR RATHER THAN A HASH SET, deliberately. The reverse-edge lookup is what the set
    // would be for, and a binary search over a few dozen entries costs no more; what the vector
    // buys is a **defined iteration order**, so the faces come out in the same order on every
    // standard library. Hull output feeding a cook step whose result depends on which
    // implementation built it is precisely the kind of divergence `simulation-and-determinism`
    // exists to prevent, and it would surface as an asset hash that differs between two machines.
    scratch.edges.clear();
    for (const u32 face_index : scratch.visible) {
        const Face& face = faces[face_index];
        scratch.edges.push_back(edge_key(face.a, face.b));
        scratch.edges.push_back(edge_key(face.b, face.c));
        scratch.edges.push_back(edge_key(face.c, face.a));
    }
    std::ranges::sort(scratch.edges);

    Face stitched;
    scratch.additions.clear();
    for (const u64 key : scratch.edges) {
        const u32 from = static_cast<u32>(key >> 32);
        const u32 to = static_cast<u32>(key & 0xFFFFFFFFu);
        if (std::ranges::binary_search(scratch.edges, edge_key(to, from))) {
            continue;
        }
        // The horizon edge keeps its direction, so the new face's winding continues the winding of
        // the surviving face on the other side of it and the hull stays consistently outward.
        if (make_face(points, from, to, index, stitched)) {
            scratch.additions.push_back(stitched);
        }
    }
    if (scratch.additions.empty()) {
        return false;
    }

    // Drop the doomed faces high index to low, swapping each with the last live face. `visible` is
    // ascending because a forward scan built it, so every index above the one being removed has
    // already been dealt with and the face swapped down from the back is never itself doomed.
    for (usize i = scratch.visible.size(); i-- > 0;) {
        faces[scratch.visible[i]] = faces.back();
        faces.pop_back();
    }
    faces.insert(faces.end(), scratch.additions.begin(), scratch.additions.end());
    return true;
}

}  // namespace

Expected<usize, Error> convex_hull_3d(const Vec3* points, usize count, f32 relative_tolerance,
                                      u32* out_indices, usize out_capacity) noexcept {
    if (const Expected<void, Error> checked =
            check_hull_arguments(points, count, out_indices, out_capacity);
        !checked) {
        return cy::make_unexpected(checked.error());
    }

    std::vector<Vec3d> doubled;
    doubled.reserve(count);
    for (usize i = 0; i < count; ++i) {
        doubled.push_back(to_double(points[i]));
    }
    const f64 epsilon = cloud_epsilon(doubled, relative_tolerance);

    u32 seed[4] = {0, 0, 0, 0};
    if (!find_seed_tetrahedron(doubled.data(), count, epsilon, seed)) {
        return cy::fail(ErrorCode::InvalidArgument,
                        "convex_hull_3d(): the points have no volume — they are coincident, "
                        "collinear or coplanar within the tolerance");
    }

    std::vector<Face> faces;
    faces.reserve(2 * count);
    build_seed_faces(doubled.data(), seed, faces);
    if (faces.size() != 4) {
        return cy::fail(ErrorCode::Internal,
                        "convex_hull_3d(): the seed tetrahedron came out degenerate");
    }

    HullScratch scratch;
    for (usize i = 0; i < count; ++i) {
        const u32 index = static_cast<u32>(i);
        if (index == seed[0] || index == seed[1] || index == seed[2] || index == seed[3]) {
            continue;
        }
        (void)add_point(doubled.data(), index, epsilon, faces, scratch);
    }

    // Euler bounds a triangulated hull at 2n - 4 faces, which is what the capacity check above
    // reserved for. Exceeding it means the stitch lost the manifold, and saying so is better than
    // writing past a caller's buffer.
    if (3 * faces.size() > out_capacity) {
        return cy::fail(ErrorCode::Internal,
                        "convex_hull_3d(): produced more faces than Euler's formula allows");
    }

    usize written = 0;
    for (const Face& face : faces) {
        out_indices[written++] = face.a;
        out_indices[written++] = face.b;
        out_indices[written++] = face.c;
    }
    return written;
}

}  // namespace cy::geom
