#pragma once
// The cook-time build: a source mesh becomes clusters, groups, a crack-free hierarchy and pages.
// M7 tasks 7.1, 7.2 and 7.5.
//
// `virtual-geometry` — "Cluster hierarchy and grouping": "Clusters SHALL be organised into a
// **hierarchy** built by iterative simplification: neighbouring clusters are collected into
// **groups**, each group is simplified as a unit, and the result becomes a smaller set of parent
// clusters", with simplification "performed **per group, not per cluster**, with shared boundary
// vertices constrained".
//
// ================================================================================================
// EVERYTHING HERE IS DETERMINISTIC, AND THAT IS TASK 7.5 RATHER THAN A NICETY
// ================================================================================================
//
// `asset-import-pipeline` requires a cook whose output is byte-identical between runs, and the
// derivation key that addresses it is only worth computing if it does. Three places in this build
// could have been non-deterministic and are not:
//
//   * clustering orders triangles by a Morton code over the mesh bounds and breaks ties by triangle
//     index, never by a hash iteration order;
//   * grouping picks the adjacent cluster with the most shared edges and breaks ties by cluster
//     index;
//   * simplification collapses edges in ascending (cost, vertex, vertex) order, recomputed in
//     passes, rather than from a priority queue whose float ties depend on insertion order.
//
// The suite asserts it by building the same mesh twice and comparing the encoded asset byte for
// byte, and `just content-validate`'s two-run gate is the same claim at the level of a package.
//
// ================================================================================================
// WHAT IS INTEGRATED AND WHAT IS OWNED
// ================================================================================================
//
// "Bounded algorithms MAY be integrated — mesh simplification and cluster generation — behind
// engine-owned interfaces, with the hierarchy, error metric, and page format remaining
// engine-owned." Both algorithms are engine code here, and the reason is the boundary rather than
// the arithmetic: crack-freeness depends on the simplifier honouring an exact locked-vertex set,
// and a library that merely *tends* to preserve boundaries would make the invariant a hope. The
// interface is nonetheless the one a library would be dropped into — `simplify_group()` in
// simplify.h takes the group's triangles and the locked set and returns a triangle list and an
// error — so replacing it is a file rather than a redesign.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/virtual_geometry/cluster.h>

namespace cy::rendering::vg {

/// The source mesh a cook hands the builder. Positions are required; the rest are optional and an
/// empty span means the attribute is absent rather than zero.
///
/// Non-owning: every span points at the caller's storage and must outlive the call.
struct SourceMesh {
    Span<const Vec3> positions;
    Span<const Vec3> normals;
    Span<const Vec2> uvs;
    Span<const u32> indices;
    /// One material index per triangle. Empty means every triangle is material 0. Clustering never
    /// mixes materials, so this partitions the mesh before anything else happens.
    Span<const u32> triangle_materials;
};

/// What the build was asked to do, beyond the cluster policy.
struct BuildOptions {
    ClusterPolicy policy;
    DeformationClass deformation = DeformationClass::Static;
    SurfaceClass surface = SurfaceClass::Solid;
    TangentPolicy tangents = TangentPolicy::Derived;

    /// Positions within this distance of each other are one vertex for the purposes of adjacency,
    /// boundary detection and locking. It is NOT a simplification tolerance: welded vertices keep
    /// their own attributes and their own indices in the output. A mesh whose clusters disagreed
    /// about which vertices they share would have no boundary to lock, so this is what makes
    /// crack-freeness reachable on an imported mesh at all.
    f32 weld_epsilon = 1.0e-5F;

    /// Bytes. `virtual-geometry`: "Page size SHALL be configurable and benchmarked rather than
    /// fixed; the specification requires it to be a policy, and the chosen size and its rationale
    /// SHALL be recorded." See README.md for the rationale behind 128 KiB.
    u32 page_bytes = 128U * 1024U;

    /// Bytes. The always-resident root region is filled with the coarsest levels until adding the
    /// next whole page would exceed this. `virtual-geometry`: "a small **always-resident** region:
    /// the hierarchy root and the coarsest clusters, sufficient to render the object recognisably".
    u32 resident_budget_bytes = 64U * 1024U;

    /// Quantisation. `virtual-geometry` — "Geometry compression": positions relative to the page's
    /// bounds, normals octahedral at a declared bit depth, UVs relative to a per-cluster range.
    u32 position_bits = 16;
    u32 normal_bits = 10;
    u32 uv_bits = 12;
};

/// One group of clusters, and the level it produced.
struct Group {
    u32 level = 0;
    /// Clusters that were members of this group, by index into `GeometryBuild::clusters`.
    u32 first_member = 0;
    u32 member_count = 0;
    /// Clusters this group was simplified into.
    u32 first_child = 0;
    u32 child_count = 0;
    f32 error = 0.0F;
    ErrorSphere sphere;
    /// Vertices held fixed while this group was simplified: the group's own boundary in the welded
    /// index space. Reported because "the boundary did not move" is the crack-free invariant and a
    /// count of zero would mean the lock did nothing.
    u32 locked_vertices = 0;
    /// Clusters at this group's level that were NOT members of it — the neighbours its boundary
    /// has to join. Recorded because it is the only thing that makes a `locked_vertices` of zero
    /// legitimate: a group holding every remaining cluster of a closed surface has no neighbour to
    /// crack against, and locking anything in it would only stop the hierarchy reaching a root.
    /// Without this field a test cannot tell that case from a lock that did not happen.
    u32 outside_clusters = 0;
};

/// One page: a run of clusters, the bytes they encode to, and the content hash that addresses them.
struct PageDescription {
    u32 first_cluster = 0;
    u32 cluster_count = 0;
    u32 byte_offset = 0;
    u32 byte_size = 0;
    /// The coarsest level in this page. The root region is the pages whose levels are highest.
    u8 max_level = 0;
    bool resident = false;
    /// BLAKE3 over the page's own bytes and nothing else, so two assets that share geometry share
    /// a page. Filled by the serialiser, which is what knows the bytes.
    u8 content_hash[32] = {};
};

/// What a build produced, before it is serialised.
///
/// The vertex and index arrays are the WHOLE asset's, in page order: a page owns a contiguous run
/// of clusters and each cluster a contiguous run of indices, so a page's payload is a slice rather
/// than a gather.
struct GeometryBuild {
    explicit GeometryBuild(Allocator& allocator) noexcept;

    GeometryBuild(const GeometryBuild&) = delete;
    GeometryBuild& operator=(const GeometryBuild&) = delete;
    GeometryBuild(GeometryBuild&&) noexcept = default;
    GeometryBuild& operator=(GeometryBuild&&) noexcept = default;

    Array<Vec3> positions;
    Array<Vec3> normals;
    Array<Vec2> uvs;
    /// Cluster-local: an index into the cluster's own vertex run, so it fits in a byte.
    Array<u32> indices;
    Array<Cluster> clusters;
    Array<Group> groups;
    /// Group membership and children, flattened. `Group::first_member` indexes this.
    Array<u32> group_members;
    Array<u32> group_children;
    /// `Cluster::first_child` indexes this.
    Array<u32> cluster_children;
    Array<PageDescription> pages;

    Aabb bounds;
    ClusterPolicy policy;
    /// Carried from `BuildOptions` because the serialiser packs the pages — it is the only stage
    /// that knows how many bytes a cluster encodes to — and must not be handed the options twice.
    u32 page_bytes = 128U * 1024U;
    u32 resident_budget_bytes = 64U * 1024U;
    DeformationClass deformation = DeformationClass::Static;
    SurfaceClass surface = SurfaceClass::Solid;
    TangentPolicy tangents = TangentPolicy::Derived;

    /// `virtual-geometry` — "Authoring experience": "Import SHALL report: source triangle count,
    /// cluster count, hierarchy depth, cooked size, resident size, bytes per triangle, and any
    /// warnings about content suitability."
    u32 source_triangles = 0;
    u32 levels = 0;
    u32 resident_pages = 0;
    u32 resident_bytes = 0;
    u32 cooked_bytes = 0;
    /// Per-cluster metadata cost, which "SHALL be reported, since it is paid for every cluster in
    /// every asset".
    u32 cluster_metadata_bytes = 0;
    f32 bytes_per_triangle = 0.0F;
    /// The largest distance any position moved when it was quantised, in world units.
    f32 quantisation_error = 0.0F;
    /// Bytes saved by not storing tangents, under the declared policy.
    u32 tangent_bytes_saved = 0;
    /// Set when the surface class is one virtual geometry serves poorly. The cook reports it rather
    /// than producing degenerate clusters silently, which is a scenario of its own.
    bool suitability_warning = false;
    const char* suitability_reason = "";
    /// Set when `ClusterPolicy::max_levels` stopped the hierarchy before it reached the root limit.
    bool level_limit_reached = false;
};

/// Build the hierarchy. The whole of task 7.1 in one call, because every stage of it needs the
/// welded adjacency the first stage computes and splitting it would mean recomputing that or
/// exposing it.
[[nodiscard]] Expected<GeometryBuild, Error> build_geometry(
    const SourceMesh& mesh, const BuildOptions& options,
    Allocator& allocator = current_allocator()) noexcept;

/// What the watertightness check found. `virtual-geometry` — "Watertightness is tested": "WHEN an
/// asset is cooked THEN an automated check SHALL verify that adjacent clusters across levels share
/// consistent boundaries."
struct WatertightReport {
    /// Groups whose boundary edge set changed under simplification. Any non-zero value is a crack.
    u32 boundary_mismatches = 0;
    /// DAG edges where the parent's error was not strictly greater than the child's, or where the
    /// parent's sphere did not contain the child's. Either produces a hole at some threshold.
    u32 monotonicity_violations = 0;
    /// Distinct thresholds swept, and how many of them produced a cut whose triangles did not form
    /// a closed surface. Only meaningful for a closed source mesh; `closed_source` says whether it
    /// was, so a check on an open mesh reports "not applicable" rather than a false pass.
    u32 thresholds_tested = 0;
    u32 open_cuts = 0;
    bool closed_source = false;

    [[nodiscard]] bool watertight() const noexcept {
        return boundary_mismatches == 0 && monotonicity_violations == 0 && open_cuts == 0;
    }
};

/// Check the build. Called by the cook — the requirement is that cooking runs this, not that a test
/// does — and by the suite over a mesh whose closure makes the cut check meaningful.
[[nodiscard]] Expected<WatertightReport, Error> check_watertight(
    const GeometryBuild& build, Allocator& allocator = current_allocator()) noexcept;

}  // namespace cy::rendering::vg
