#ifndef CY_IMPORT_MESH_H
#define CY_IMPORT_MESH_H
// Mesh processing: the reusable steps every model importer needs. M5 task 5.1.
//
// `asset-import-pipeline` — "Mesh processing": "Mesh processing SHALL provide, as reusable steps
// available to importers and to runtime tools: vertex welding within position, normal and UV
// tolerances; normal generation with a smoothing angle, and tangent generation with a documented
// convention; vertex cache optimisation, overdraw optimisation, and vertex fetch optimisation; LOD
// simplification with error bounds, seam and boundary preservation; UV2 generation ...; convex hull
// generation and convex decomposition for collision."
//
// --- WHERE THE LIBRARY IS, AND WHY IT IS NOT LINKED YET ------------------------------------------
//
// `thirdparty-dependencies` names **meshoptimizer** for this work and it is the right dependency:
// its simplifier and its cache optimiser are better than what is below, and neither is
// differentiating. It is not in `deps/manifest.toml` at M5, and that is a deliberate, stated choice
// rather than an omission — see the note at the head of that file. The shape here is the one
// `physics` used: the ENGINE-OWNED INTERFACE FIRST, with an implementation good enough for the
// pipeline to be exercised end to end, and the library behind the same interface when it is
// integrated. Every function below is a free function over `MeshData`, so swapping an
// implementation is a change to one `.cpp` and to nothing that calls it.
//
// What that costs, stated: `simplify` is a quadric-error edge collapse with a heap, which is the
// same algorithm meshoptimizer uses and a less tuned implementation of it. Expect worse output at
// aggressive ratios and comparable output at the 50-70% an LOD chain's first rungs use.
//
// --- THE TANGENT CONVENTION, WRITTEN DOWN ONCE ---------------------------------------------------
//
// "WHEN tangents are generated THEN they SHALL follow the documented convention matching the normal
// map convention, so imported and generated tangents agree."
//
// The convention is `cy::geom::generate_tangents`': right-handed, `+X` in tangent space is the
// direction of increasing U, `+Y` is increasing V, `w` is ±1 and the bitangent is
// `cross(normal, tangent.xyz) * tangent.w`. That is glTF's convention and MikkTSpace's sign
// convention, which is what makes an imported tangent and a generated one agree — the case the
// scenario is about. Normal maps are therefore expected in OpenGL orientation (+Y up); the texture
// importer's `normal-convention` option is what converts a DirectX-orientation map, and it converts
// the TEXTURE rather than the tangents, because a per-mesh fix would leave the two disagreeing for
// any material shared between meshes.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

#include <vector>

namespace cy::import {

/// One contiguous run of indices drawn with one material. The importer splits by material here
/// rather than producing one mesh per material, so that a model with eight materials is one vertex
/// buffer and eight draws instead of eight of everything.
struct MeshSection {
    u32 first_index = 0;
    u32 index_count = 0;
    /// The index of the material in the import's own material list. Resolved to an `AssetId` by the
    /// importer that produced both.
    u32 material = 0;
};

/// A mesh as the importer holds it: parallel attribute arrays and a triangle index list.
///
/// Deliberately NOT the cooked layout. A cooked mesh is interleaved, quantised and split into
/// streams for the renderer's own vertex fetch; that is `rendering-geometry-and-resources`' shape
/// and this is the shape processing steps are written against. Keeping them apart means a change to
/// the cooked layout does not touch the simplifier.
struct MeshData {
    Array<Vec3> positions;
    /// Empty until `generate_normals` runs or the source supplied them.
    Array<Vec3> normals;
    /// The first texture coordinate set. Empty when the source has none.
    Array<Vec2> uvs;
    /// The lightmap coordinate set. Empty until it is generated or supplied.
    Array<Vec2> uv2;
    /// `xyz` is the tangent and `w` the handedness. See the convention note at the top.
    Array<Vec4> tangents;
    /// Triangles, three indices each.
    Array<u32> indices;
    Array<MeshSection> sections;

    [[nodiscard]] usize vertex_count() const noexcept { return positions.size(); }
    [[nodiscard]] usize triangle_count() const noexcept { return indices.size() / 3; }

    /// Whether the arrays agree with each other: every non-empty attribute has one entry per
    /// vertex, the index count is a multiple of three, every index is in range, and the sections
    /// tile the index list without overlapping. Called by every step below before it starts,
    /// because a step that assumed a well-formed mesh and got a malformed one reads out of bounds.
    [[nodiscard]] Status validate() const noexcept;

    /// The axis-aligned bounds of the positions. An empty mesh has an inverted box, which is what
    /// `Aabb`'s own empty state is.
    [[nodiscard]] Aabb bounds() const noexcept;

    /// Drop every attribute and index.
    void clear() noexcept;
};

/// How aggressively `weld` merges.
///
/// Three tolerances rather than one, because the requirement names three and because they are not
/// interchangeable: two vertices a millimetre apart are the same vertex, two whose normals differ
/// by 80 degrees are a hard edge that must stay split, and two whose UVs differ are a texture seam
/// that must stay split even though everything else about them matches.
struct WeldOptions {
    f32 position_tolerance = 1.0e-5f;
    /// Degrees. Normals further apart than this are not merged, which is what keeps a hard edge
    /// hard.
    f32 normal_tolerance_degrees = 1.0f;
    f32 uv_tolerance = 1.0e-5f;
};

/// Merge vertices that agree within the tolerances, rewriting the indices.
///
/// Reports how many vertices were removed. Attribute arrays that were empty stay empty; an
/// attribute that exists takes the FIRST merged vertex's value rather than an average, so welding
/// is idempotent — welding twice produces the same mesh, which a pipeline that re-runs steps
/// depends on.
[[nodiscard]] Expected<usize, Error> weld(MeshData& mesh, const WeldOptions& options) noexcept;

/// Generate per-vertex normals, splitting vertices where the angle between adjacent faces exceeds
/// `smoothing_angle_degrees`.
///
/// Area-weighted rather than uniform: a large triangle should influence a shared normal more than a
/// sliver, and the alternative makes a badly tessellated mesh shade as if its topology were
/// visible.
///
/// A smoothing angle of 180 makes every vertex smooth and never splits; 0 makes every face flat.
[[nodiscard]] Status generate_normals(MeshData& mesh, f32 smoothing_angle_degrees) noexcept;

/// Generate tangents from positions, normals and UVs, in the convention at the top of this file.
///
/// Fails with `InvalidArgument` when the mesh has no normals or no UVs: a tangent basis without
/// either is not a thing that can be computed, and producing an arbitrary one would be a normal map
/// that is subtly wrong everywhere rather than an error somebody fixes.
[[nodiscard]] Status generate_tangents(MeshData& mesh) noexcept;

/// Reorder triangles so that a vertex is reused while it is still in the post-transform cache.
///
/// Tom Forsyth's linear-speed algorithm: a per-vertex score from its cache position and its
/// remaining triangle count, and repeated selection of the highest-scoring triangle. It is O(n), it
/// needs no tuning per hardware, and it gets within a few percent of the best known results — which
/// is why it is still the algorithm everybody uses twenty years on.
///
/// Vertices are untouched; only the index list is permuted. Sections are permuted with it, so a
/// mesh with several materials stays correct.
[[nodiscard]] Status optimise_vertex_cache(MeshData& mesh) noexcept;

/// Reorder vertices into the order the indices first reference them.
///
/// Run AFTER `optimise_vertex_cache`, which is what makes it worth anything: it turns the index
/// list's access pattern into a mostly-forward scan of the vertex buffer, which is the difference
/// between a vertex fetch that streams and one that scatters. Running it first optimises for an
/// order that is about to change.
[[nodiscard]] Status optimise_vertex_fetch(MeshData& mesh) noexcept;

/// What `simplify` is aiming for.
struct SimplifyOptions {
    /// The share of the original triangles to keep, in (0, 1]. 0.5 halves the mesh.
    f32 target_ratio = 0.5f;
    /// Stop before a collapse whose quadric error exceeds this, even if the ratio is not reached.
    ///
    /// This is what makes an LOD chain safe to generate unattended: a mesh that cannot be halved
    /// without visible damage comes back larger than asked for, rather than damaged. Zero means no
    /// bound, which is what a caller that has already decided asks for.
    f32 error_bound = 0.0f;
    /// Keep geometric boundaries — an open edge, the rim of a plane — where they are.
    bool preserve_boundary = true;
    /// Keep attribute seams: two vertices at the same position with different UVs or normals.
    ///
    /// "WHEN a mesh with UV seams is simplified THEN seam vertices SHALL be preserved or collapsed
    /// only along the seam, avoiding texture distortion." With this on, a seam edge may collapse
    /// along the seam and may not collapse across it.
    bool preserve_seams = true;
};

/// What `simplify` did.
struct SimplifyReport {
    usize triangles_before = 0;
    usize triangles_after = 0;
    usize vertices_before = 0;
    usize vertices_after = 0;
    /// The largest quadric error any accepted collapse incurred. Comparable across runs of the same
    /// mesh and not across meshes, because a quadric is in squared world units.
    f32 error = 0.0f;
    /// True when the error bound stopped it before the target ratio was reached.
    bool bounded = false;
};

/// Reduce the triangle count by quadric-error edge collapse.
///
/// Deterministic: ties are broken by the edge's own index, so two runs on the same machine and on
/// two machines produce the same mesh. That is not a nicety — `asset-import-pipeline` requires
/// byte-identical output for identical input, and a simplifier that broke ties by heap address
/// would quietly make every cook cache miss.
[[nodiscard]] Expected<SimplifyReport, Error> simplify(MeshData& mesh,
                                                       const SimplifyOptions& options) noexcept;

/// The convex hull of a mesh's positions, as a triangle mesh, for collision.
///
/// `physics`' hull budget is the caller's: this produces the exact hull and reports its vertex
/// count, and a caller that wants at most N planes simplifies the result. Derived from the SOURCE
/// positions rather than from any LOD, per "Collision is independent".
[[nodiscard]] Status convex_hull(const MeshData& mesh, MeshData& out) noexcept;

// --- Overdraw ------------------------------------------------------------------------------------

/// Reorder triangles so that, from any direction, the ones nearest the viewer tend to be drawn
/// first — which is what lets early-Z reject the ones behind them. M6 task 8.2.
///
/// `asset-import-pipeline` — "Mesh processing" names "vertex cache optimisation, overdraw
/// optimisation, and vertex fetch optimisation" as three separate steps, and only two of them
/// existed at M5.
///
/// WHAT IT DOES, AND WHAT IT COSTS. `optimise_vertex_cache` has already grouped the index list into
/// runs of triangles that reuse each other's vertices; breaking those runs to sort by depth would
/// win overdraw and lose more to cache misses. So this step keeps the runs and reorders THEM,
/// sorting each run by the distance from the mesh's centroid to the run's own centroid — nearer
/// first — and it accepts a run only while the cache efficiency it would give up stays within
/// `threshold` of the input's. A threshold of 1.0 forbids any regression and does almost nothing; 3
/// is the value that is worth having; below 1.0 the argument is rejected.
///
/// Run AFTER `optimise_vertex_cache` and BEFORE `optimise_vertex_fetch`, which is the order every
/// step's own precondition already implies: the first produces the runs this permutes, and the last
/// renumbers vertices into the final index order.
///
/// Deterministic. Ties are broken by the run's first index, never by a sort that is not stable,
/// because two cooks of one mesh must produce one file.
[[nodiscard]] Status optimise_overdraw(MeshData& mesh, f32 threshold) noexcept;

// --- Lightmap coordinates ------------------------------------------------------------------------

/// What `generate_uv2` is aiming for. M6 task 8.2.
///
/// `asset-import-pipeline` — "Mesh processing": "UV2 generation with configurable texel density,
/// chart padding, and distortion limits."
struct Uv2Options {
    /// Texels per world unit. The atlas is sized from it and from the mesh's own surface area, so a
    /// large object gets a large atlas at the same density rather than the same atlas at a lower
    /// one — which is the property that makes one number describe a whole project.
    f32 texel_density = 16.0f;
    /// Texels left between charts, so a bilinear tap at a chart's edge cannot reach its neighbour.
    /// Two is enough for bilinear; a lightmap that is also mip-mapped wants more.
    u32 padding = 2;
    /// The stretch a chart may carry before it is cut, as the ratio of the parameterised area to
    /// the world area. 1.0 admits no stretch at all and produces a chart per triangle; the default
    /// is the value xatlas itself defaults to.
    f32 max_distortion = 2.0f;
    /// Force the atlas's width and height instead of deriving them from `texel_density`. Zero
    /// derives them, which is the normal case.
    u32 resolution = 0;
    /// The angle in degrees beyond which two adjacent faces are a chart boundary.
    f32 max_chart_angle = 88.0f;
};

/// What `generate_uv2` produced.
struct Uv2Report {
    u32 charts = 0;
    u32 width = 0;
    u32 height = 0;
    /// Vertices the unwrap had to split, because a vertex on a chart boundary needs one UV per
    /// chart. The mesh's other attribute arrays are duplicated with it, so this is the amount by
    /// which the vertex buffer grew.
    usize vertices_added = 0;
    /// The share of the atlas the charts occupy, in (0, 1]. A low number on a large atlas is the
    /// signal that the density is too high for the shape.
    f32 utilisation = 0.0f;
};

/// Generate the lightmap coordinate set, replacing whatever `MeshData::uv2` held.
///
/// This is the one step whose implementation is a third-party library rather than engine-owned:
/// chart segmentation, parameterisation and packing are `thirdparty-dependencies`' xatlas entry,
/// and `tools/import/src/unwrap.cpp` is the only translation unit in the tree that names an xatlas
/// symbol.
///
/// UNWRAPPING CHANGES THE VERTEX BUFFER. A chart boundary is a UV discontinuity, so vertices on it
/// are split and every other attribute array is duplicated along with them; the index list is
/// rewritten and the sections are preserved. That is why this runs on a copy of the mesh in
/// `finish_mesh`'s order — after welding and before the LOD chain, so every level inherits a
/// consistent UV2 rather than each level being unwrapped separately into a different atlas.
///
/// Fails with `InvalidArgument` on a mesh with no triangles or with options outside their ranges.
[[nodiscard]] Expected<Uv2Report, Error> generate_uv2(MeshData& mesh,
                                                      const Uv2Options& options) noexcept;

// --- Convex decomposition ------------------------------------------------------------------------

/// What `convex_decomposition` is allowed to spend. M6 task 8.2.
struct ConvexDecompositionOptions {
    /// The most parts to produce. The budget `physics` requires a collision representation to hold.
    u32 max_parts = 8;
    /// Stop splitting a part once the volume its hull adds over the part's own bounds falls below
    /// this share. 0.05 is "the hull is within five per cent of the shape".
    f32 concavity_tolerance = 0.05f;
    /// A part with fewer triangles than this is never split again.
    u32 min_triangles = 12;
};

/// Decompose a mesh into a small set of convex hulls that together approximate it.
///
/// `asset-import-pipeline` — "Mesh processing": "convex hull generation and convex decomposition
/// for collision". A single hull is wrong for anything with a hole or a concavity a character can
/// stand in, and a triangle mesh is the collision representation a dynamic body may not have.
///
/// WHAT THIS IS, STATED PLAINLY. Recursive bisection: measure how far the part's own hull departs
/// from the part, split along the axis of its bounding box that carries the most of that departure,
/// and recurse until the budget or the tolerance stops it. It is not V-HACD — it does not voxelise
/// and it does not search plane orientations — and on a shape whose concavity is not axis-aligned
/// it produces more parts than V-HACD would for the same fidelity. It is deterministic, it has no
/// dependency, and its output is a set of hulls a solver can use, which is what the requirement
/// asks for. A project that needs V-HACD's quality integrates V-HACD behind this signature.
///
/// The parts are derived from the SOURCE mesh, never from a level of detail — "Collision is
/// independent" — and are appended to `out` in a deterministic order: the split's near side before
/// its far side, depth first.
[[nodiscard]] Expected<usize, Error> convex_decomposition(const MeshData& mesh,
                                                          const ConvexDecompositionOptions& options,
                                                          std::vector<MeshData>& out) noexcept;

}  // namespace cy::import

#endif  // CY_IMPORT_MESH_H
