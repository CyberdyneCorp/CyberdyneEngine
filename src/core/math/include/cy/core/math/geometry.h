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
// WHAT IS NOT YET HERE, and is therefore not claimed: 3D convex hull, Delaunay triangulation,
// polygon boolean operations and polygon offsetting. All four are named by the specification and
// none is implemented — see src/core/math/README.md. The four that are here (2D hull, ear-clipping
// triangulation, plane-set clipping, atlas packing) are the ones the milestone's own consumers
// need, and each of the missing four is a substantial algorithm that deserves its own tests rather
// than a hurried version underneath somebody else's deadline.

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
