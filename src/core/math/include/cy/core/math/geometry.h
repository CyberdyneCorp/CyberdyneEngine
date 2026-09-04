#pragma once
// Geometry utilities: intersection, closest points, hulls, triangulation, clipping, atlas packing
// and mesh preparation. Task 3.1.5.
//
// `core-math` — "Geometry utilities". Everything here is a free function over plain data: no
// object owns a mesh, and nothing here allocates unless it says so in its signature. Routines that
// produce a variable number of results take an output buffer and a capacity and report
// `ErrorCode::BufferTooSmall`, which is what lets them be called from a job with a scratch
// allocation rather than from a call site that can afford a vector.
//
// A "no intersection" answer is a `bool`, not an `Error`. A ray missing a box is the ordinary case
// and the overwhelmingly common one; making it an error would put a diagnostic path under the
// hottest loop in the picking and physics code.
//
// EVERY ROUTINE THE SPECIFICATION NAMES IS HERE, AND EACH STATES WHAT IT DOES NOT HANDLE. The four
// that arrive last — 3D convex hull, Delaunay triangulation, polygon booleans and polygon
// offsetting — are the ones with genuine degenerate cases, and each of them answers a degeneracy it
// cannot resolve with an `Error` naming it rather than with a plausible-looking wrong polygon. A
// boolean operation that quietly drops a contour when two edges are collinear is the single most
// expensive kind of bug in this file: the caller has no way to notice, and the damage surfaces
// three subsystems away.
//
// Their implementations live one algorithm to a source file — src/hull3d.cpp, src/delaunay.cpp,
// src/polygon_boolean.cpp, src/polygon_offset.cpp — because each is a named algorithm with its own
// robustness argument, and a reader checking that argument should not have to find it inside a
// nine-hundred-line file.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>

#include <vector>

namespace cy::geom {

// --- Ray casts
// ------------------------------------------------------------------------------------
//
// `t` is the distance along the ray in metres, which is only true while the ray's direction is
// unit-length; every routine here assumes it is. Hits behind the origin are not reported: the
// interval tested is [0, t_max].

/// Where a ray first meets a triangle, plus where on the triangle it landed.
struct TriangleHit {
    f32 t = 0.0f;
    /// Barycentric coordinates of the hit with respect to (v0, v1, v2): the weight of v0 is
    /// `1 - u - v`. This is what the editor's picking path interpolates a UV or a vertex colour
    /// with, and it is why the routine returns them rather than only a position.
    f32 u = 0.0f;
    f32 v = 0.0f;
    /// True when the ray struck the back of the triangle — the side its winding faces away from.
    bool back_face = false;
};

/// Slab test. `t_min` receives the entry distance (0 when the origin is inside) and `t_max` the
/// exit distance.
[[nodiscard]] bool ray_aabb(const Ray& ray, const Aabb& box, f32 max_distance, f32& t_min,
                            f32& t_max) noexcept;

/// `t` receives the nearer non-negative root: the entry point, or the exit point when the origin is
/// inside the sphere.
[[nodiscard]] bool ray_sphere(const Ray& ray, const Sphere& sphere, f32 max_distance,
                              f32& t) noexcept;

/// A ray against an infinite plane. A ray parallel to the plane never hits, including when it lies
/// in the plane — an infinite set of hits is not a hit anyone can use.
[[nodiscard]] bool ray_plane(const Ray& ray, const Plane& plane, f32 max_distance, f32& t) noexcept;

/// Möller–Trumbore. `cull_back_faces` skips triangles whose winding faces away from the ray, which
/// is what a solid mesh wants and what a two-sided leaf card does not.
[[nodiscard]] bool ray_triangle(const Ray& ray, Vec3 v0, Vec3 v1, Vec3 v2, f32 max_distance,
                                bool cull_back_faces, TriangleHit& hit) noexcept;

// --- Closest points
// --------------------------------------------------------------------------------

/// The closest pair of points on two segments, and their parameters. Handles the degenerate cases —
/// either segment of zero length, and the two parallel — without a special case at the call site.
void closest_points_segment_segment(Vec3 a_start, Vec3 a_end, Vec3 b_start, Vec3 b_end, f32& s,
                                    f32& t, Vec3& point_a, Vec3& point_b) noexcept;

/// The closest point on a segment to `p`.
[[nodiscard]] Vec3 closest_point_on_segment(Vec3 p, Vec3 start, Vec3 end) noexcept;

/// The closest point on a triangle to `p`, by Ericson's region test.
[[nodiscard]] Vec3 closest_point_on_triangle(Vec3 p, Vec3 v0, Vec3 v1, Vec3 v2) noexcept;

// --- Polygons
// ---------------------------------------------------------------------------------------

/// Even-odd (crossing-number) containment for a simple polygon, in 2D. A point exactly on an edge
/// may answer either way; a caller that needs a decision on the boundary should test the distance
/// to the edge explicitly rather than relying on the tie.
[[nodiscard]] bool point_in_polygon(Vec2 point, const Vec2* vertices, usize count) noexcept;

/// Twice the signed area of a simple polygon. Positive for counter-clockwise winding, which is the
/// engine's front-facing convention.
[[nodiscard]] f32 polygon_signed_area2(const Vec2* vertices, usize count) noexcept;

/// The 2D convex hull, by Andrew's monotone chain: sort, then two passes.
///
/// Writes the indices of the hull vertices into `out_indices` in counter-clockwise order and
/// returns how many there were. Collinear points on a hull edge are dropped, so the result is the
/// minimal hull.
///
/// `out_indices` must have room for `count + 1`: the hull itself is at most `count` vertices, and
/// the monotone chain's second pass transiently holds one more. `scratch` must have room for
/// `count` indices, which is where the routine sorts rather than allocating.
[[nodiscard]] Expected<usize, Error> convex_hull_2d(const Vec2* points, usize count,
                                                    u32* out_indices, usize out_capacity,
                                                    u32* scratch) noexcept;

/// The convex hull of a 3D point cloud, as outward-facing triangles. Incremental (quickhull's
/// construction without its recursion): seed a tetrahedron from four extreme points, then add each
/// remaining point by deleting the faces it can see and stitching new ones to the horizon.
///
/// Writes three indices per triangle into `out_indices` and returns the number of **indices**.
/// Triangles are counter-clockwise seen from outside, so `cross(b - a, c - a)` points away from the
/// hull's interior. Euler's formula bounds a triangulated hull at `2n - 4` faces, so
/// `out_capacity` must be at least `6 * count`.
///
/// `relative_tolerance` is scaled by the cloud's own extent to give the distance below which a
/// point counts as lying *on* the hull rather than outside it; `1e-5f` is a reasonable default for
/// engine-scale geometry. A point within that distance of the surface is treated as inside, which
/// is what keeps a nearly-coplanar cluster from generating slivers, and it means the result is the
/// minimal hull only up to that tolerance.
///
/// Reports `ErrorCode::InvalidArgument` when the cloud has no volume — fewer than four points, or
/// all of them collinear or coplanar within the tolerance. A flat cloud is not a 3D hull with zero
/// thickness, it is a 2D hull in a plane, and answering it here would mean returning a
/// zero-area triangle fan that every consumer would then have to special-case.
///
/// Allocates a working list of faces, which peaks at the hull's own face count.
[[nodiscard]] Expected<usize, Error> convex_hull_3d(const Vec3* points, usize count,
                                                    f32 relative_tolerance, u32* out_indices,
                                                    usize out_capacity) noexcept;

/// Ear-clipping triangulation of a simple polygon (no holes, no self-intersection).
///
/// Writes `3 * (count - 2)` indices into `out_indices` and returns that number. Accepts either
/// winding and emits counter-clockwise triangles. Reports `ErrorCode::InvalidArgument` for a
/// polygon it cannot triangulate, which in practice means one that is self-intersecting or
/// degenerate — ear clipping cannot distinguish the two, and saying which would be a guess.
///
/// The one routine here that allocates: it holds a working list of the not-yet-clipped vertices,
/// which shrinks as ears are removed. A caller on a hot path should triangulate at cook time.
[[nodiscard]] Expected<usize, Error> triangulate_polygon(const Vec2* vertices, usize count,
                                                         u32* out_indices,
                                                         usize out_capacity) noexcept;

/// Delaunay triangulation of a 2D point set, by Bowyer–Watson.
///
/// Triangulates the points themselves — this is not a polygon triangulator and it ignores any
/// ordering the caller had in mind: the result covers the point set's convex hull, so a
/// non-convex boundary comes back filled in. Use `triangulate_polygon` for a polygon and this for a
/// point cloud (terrain samples, a navigation mesh's seed points, a Voronoi dual).
///
/// Writes three indices per triangle into `out_indices`, counter-clockwise, and returns the number
/// of **indices**. A triangulation of `n` points has at most `2n - 5` triangles, so `out_capacity`
/// must be at least `6 * count`.
///
/// The defining property — no point lies inside any triangle's circumcircle — is decided in
/// `double` precision even though the inputs and outputs are `f32`, because the incircle predicate
/// is a difference of nearly equal products and is the one place in this file where 32-bit
/// arithmetic flips an answer on ordinary engine-scale coordinates.
///
/// Reports `ErrorCode::InvalidArgument` for fewer than three points, for two points at exactly the
/// same coordinates (weld first: a duplicate has no Delaunay answer, only an arbitrary one), and
/// for a point set with no area — all points collinear, which has no triangulation at all.
///
/// Allocates: the working triangle list, which peaks at roughly `2n`.
[[nodiscard]] Expected<usize, Error> triangulate_delaunay(const Vec2* points, usize count,
                                                          u32* out_indices,
                                                          usize out_capacity) noexcept;

/// Sutherland–Hodgman: clip a convex polygon against one plane, keeping the half-space the normal
/// points **into** — the side where `signed_distance >= 0`. That is the same "inside" convention
/// `Frustum` uses (shapes.h), so a polygon clipped by a frustum's own planes needs no sign flip.
///
/// Returns the number of vertices written. `out_vertices` needs room for `count + 1` — a convex
/// polygon gains at most one vertex per plane.
[[nodiscard]] Expected<usize, Error> clip_polygon_by_plane(const Vec3* vertices, usize count,
                                                           const Plane& plane, Vec3* out_vertices,
                                                           usize out_capacity) noexcept;

/// Clip against a set of planes in turn, ping-ponging between two buffers. Both buffers need room
/// for `count + plane_count`. The result is left in `out_vertices` and its length returned.
[[nodiscard]] Expected<usize, Error> clip_polygon_by_planes(const Vec3* vertices, usize count,
                                                            const Plane* planes, usize plane_count,
                                                            Vec3* out_vertices, Vec3* scratch,
                                                            usize buffer_capacity) noexcept;

// --- Polygon boolean operations
// -----------------------------------------------------------------

/// What `polygon_boolean` computes. `Difference` is subject minus clip; there is no reversed
/// spelling because swapping the arguments is clearer than a fourth enumerator.
enum class BooleanOp : u32 {
    Union,
    Intersection,
    Difference,
};

/// How much `polygon_boolean` wrote. The result is a *set* of contours: intersecting two
/// L-shapes can produce two disjoint pieces, and a routine that returned one polygon would have to
/// pick one and lose the other.
struct BooleanResult {
    /// Contours written to `out_contour_sizes`. Zero is a legitimate answer — two disjoint
    /// polygons have an empty intersection — and is not an error.
    usize contour_count = 0;
    /// Vertices written to `out_vertices`, concatenated: contour *i* occupies the
    /// `out_contour_sizes[i]` entries that follow the sizes of the contours before it.
    usize vertex_count = 0;
};

/// Union, intersection or difference of two **simple** polygons, by Greiner–Hormann: intersect
/// every edge pair, thread the crossings into both polygons' vertex rings, label each as an entry
/// or an exit, and walk.
///
/// Both inputs must be simple (no self-intersection) and may be convex or not. Winding is
/// irrelevant on input; each output contour comes back counter-clockwise.
///
/// `out_vertices` needs room for `subject_count + clip_count + 2 * crossings`, which is bounded by
/// `2 * (subject_count + clip_count) + 2 * subject_count * clip_count` in the worst case and is
/// nothing like that in practice; `out_contour_sizes` needs one entry per contour.
/// `ErrorCode::BufferTooSmall` names which of the two ran out.
///
/// **DEGENERACIES ARE REFUSED, NOT GUESSED.** Greiner–Hormann is exact for polygons in general
/// position and undefined when a vertex of one polygon lies exactly on an edge of the other, when
/// two edges are collinear and overlap, or when a vertex is shared. Those cases are detected and
/// reported as `ErrorCode::Unsupported` naming the degeneracy, because the alternative — carrying
/// on and emitting a contour that is short one vertex — produces a result the caller cannot tell
/// from a correct one. Two identical squares are the shortest example, and it is a test.
///
/// One case is refused for a different reason: a difference whose clip lies strictly inside the
/// subject is an annulus, and an annulus is a contour with a hole. This interface has no way to say
/// "this contour is a hole in that one", so it reports `ErrorCode::Unsupported` rather than
/// returning the outer boundary and silently filling the hole in.
///
/// Allocates its working rings, which are proportional to the input plus the crossings.
[[nodiscard]] Expected<BooleanResult, Error> polygon_boolean(BooleanOp op, const Vec2* subject,
                                                             usize subject_count, const Vec2* clip,
                                                             usize clip_count, Vec2* out_vertices,
                                                             usize out_vertex_capacity,
                                                             u32* out_contour_sizes,
                                                             usize out_contour_capacity) noexcept;

// --- Polygon offsetting
// -------------------------------------------------------------------------

/// What to do at a corner where the two offset edges no longer meet at a point close enough to use.
enum class JoinStyle : u32 {
    /// Extend both offset edges to their intersection, unless that point is further from the
    /// original corner than `miter_limit` times the offset distance, in which case bevel.
    Miter,
    /// Always cut the corner off with a straight segment between the two offset edge ends.
    Bevel,
};

/// Move every edge of a simple polygon `distance` metres along its outward normal and rejoin the
/// corners. Positive grows the polygon, negative shrinks it.
///
/// Input must be **counter-clockwise** — `polygon_signed_area2` positive — and
/// `ErrorCode::InvalidArgument` says so when it is not. Accepting either winding would make the
/// sign of `distance` mean "outward" for one caller and "inward" for the next, which is exactly the
/// kind of convention this module exists to pin down. Reverse the vertex order first if you have a
/// clockwise polygon.
///
/// `miter_limit` is the ratio of the miter's length to the offset distance, the same quantity SVG
/// and every stroking API call by that name; 4 is the usual default and 1 forces bevels
/// everywhere. It is ignored for a reflex corner, where the two offset edges cross and their
/// intersection is the only sensible answer whatever the style.
///
/// Writes at most `2 * count` vertices — one per corner, two at a bevelled one — and returns how
/// many.
///
/// **THE RESULT MAY SELF-INTERSECT AND IS NOT CLEANED.** Shrink a polygon by more than the radius
/// of its narrowest neck and the offset edges cross over: the vertices returned are still each
/// correct, and the ring they form is no longer simple. Removing those loops is a boolean union of
/// the offset segments' swept regions, which is a different and much larger algorithm; the
/// inexpensive check a caller can make is that `polygon_signed_area2` has not changed sign.
[[nodiscard]] Expected<usize, Error> offset_polygon(const Vec2* vertices, usize count, f32 distance,
                                                    JoinStyle join, f32 miter_limit,
                                                    Vec2* out_vertices,
                                                    usize out_capacity) noexcept;

// --- Rectangle atlas packing
// --------------------------------------------------------------------------

/// A skyline bottom-left packer: the standard choice for a texture atlas or a shadow atlas.
///
/// Rectangles are placed against a piecewise-constant "skyline" of used height, at the lowest
/// position that fits and, among those, the leftmost. It is not optimal — optimal rectangle packing
/// is NP-hard — and it is within a few percent of the best known heuristics while being O(n) in the
/// skyline's width per insertion, which is what makes it usable at runtime rather than only at
/// cook time.
///
/// Insertion order matters: packing large rectangles first gives a markedly better result. The
/// packer does not sort for the caller, because a caller that needs stable slot identities (a font
/// atlas keyed by glyph) must not have its order changed underneath it.
class AtlasPacker {
public:
    AtlasPacker() = default;
    explicit AtlasPacker(IVec2 size) { reset(size); }

    /// Empty the packer and set its bounds. Existing placements are forgotten.
    void reset(IVec2 size);

    /// Place a rectangle, or report `ErrorCode::OutOfRange` when it does not fit. The returned rect
    /// is in atlas pixels.
    [[nodiscard]] Expected<IRect, Error> pack(IVec2 size);

    [[nodiscard]] IVec2 size() const noexcept { return size_; }

    /// Used area over total area, in [0, 1]. The number to look at before deciding an atlas is too
    /// small: a failure at 60% occupancy is a fragmentation problem, one at 95% is a size problem.
    [[nodiscard]] f32 occupancy() const noexcept;

private:
    struct SkylineNode {
        i32 x = 0;
        i32 y = 0;
        i32 width = 0;
    };

    /// The lowest y at which `width` pixels starting at node `index` are all free, or -1.
    [[nodiscard]] i32 fit(usize index, i32 width, i32 height) const noexcept;
    void add_level(usize index, const IRect& placed);

    IVec2 size_{0, 0};
    std::vector<SkylineNode> skyline_;
    i64 used_area_ = 0;
};

// --- Mesh utilities
// ------------------------------------------------------------------------------------

/// Merge positions that coincide within `tolerance`.
///
/// Writes, for each input vertex, the index of the unique vertex it maps to (`out_remap`, `count`
/// entries), and the unique positions themselves (`out_unique`, at most `count` entries), returning
/// the number of unique vertices.
///
/// The comparison is a hashed lattice snap rather than an all-pairs distance test, so it is O(n) —
/// and it is therefore *not* transitive: three vertices in a chain each within tolerance of the
/// next may not all merge. That is the standard trade for a welder that runs on a real mesh, and
/// the alternative is O(n²) on a million-vertex import.
[[nodiscard]] Expected<usize, Error> weld_vertices(const Vec3* positions, usize count,
                                                   f32 tolerance, u32* out_remap,
                                                   Vec3* out_unique) noexcept;

/// Per-vertex tangents from positions, normals and UVs, accumulated over the triangles and
/// orthonormalised against the normal.
///
/// `out_tangents[i].xyz` is the tangent and `.w` is the handedness — +1 or −1 — which is what a
/// shader multiplies the bitangent by. Storing handedness rather than the bitangent itself halves
/// the vertex cost and is what every modern format does.
///
/// A vertex whose triangles give a degenerate tangent (no UV variation, a zero-area triangle) gets
/// an arbitrary tangent perpendicular to its normal rather than a zero vector, because a zero
/// tangent produces a NaN in the shader's normalise and a black pixel that is very hard to trace
/// back to the mesh.
[[nodiscard]] Expected<void, Error> generate_tangents(const Vec3* positions, const Vec3* normals,
                                                      const Vec2* uvs, usize vertex_count,
                                                      const u32* indices, usize index_count,
                                                      Vec4* out_tangents) noexcept;

}  // namespace cy::geom
