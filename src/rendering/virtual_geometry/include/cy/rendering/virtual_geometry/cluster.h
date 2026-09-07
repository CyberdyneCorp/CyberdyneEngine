#pragma once
// CyberGeometry's vocabulary: the cluster, its error, its page, and the classes an asset declares.
// M7 task 7.1.
//
// `virtual-geometry` — "Clusters": "Each cluster SHALL carry at minimum: bounds, a **normal cone**
// (average normal and maximum deviation), a geometric error, its material range, hierarchy links,
// and its page." Everything in this header is that sentence, plus the two spheres the error is
// projected through and the reason there are two of them.
//
// ================================================================================================
// THE CRACK-FREE RULE IS TWO NUMBERS PER CLUSTER, AND IT IS THE WHOLE DESIGN
// ================================================================================================
//
// A cluster is selected when
//
//     screen_error(lod_error, lod_sphere) <= threshold < screen_error(parent_error, parent_sphere)
//
// and it is selected on no other grounds. Both halves are properties of a GROUP rather than of the
// cluster:
//
//   * `lod_error` / `lod_sphere` describe the group this cluster was PRODUCED FROM by simplifying
//     it. Every cluster produced from one group carries the same pair.
//   * `parent_error` / `parent_sphere` describe the group this cluster was SIMPLIFIED INTO. Every
//     cluster that was a member of one group carries the same pair.
//
// That is what makes the cut watertight, and the argument is short enough to write down. Take a
// group G, its members M (at level L) and the clusters P it was simplified into (at level L+1).
// Every m in M has `m.parent_error == e(G)`; every p in P has `p.lod_error == e(G)`. For a
// threshold t below the projection of e(G): every p fails its own lower test and is not selected,
// so traversal descends past it; every m passes its upper test and is a candidate. For a t at or
// above it: every m fails its upper test; every p passes its lower one. So G's region of the
// surface is covered EITHER by P or by M's own subtrees, never by a mixture and never by neither —
// and since simplification held G's boundary vertices fixed, P joins G's neighbours along exactly
// the edges M did.
//
// Two consequences that are easy to get wrong and are enforced in build.cpp:
//
//   1. `parent_error` must be STRICTLY greater than `lod_error`. Equal errors make the two tests
//      `e <= t` and `t < e` mutually exclusive, so no cluster on that path is selected and the
//      surface has a hole exactly at t == e.
//   2. `parent_sphere` must CONTAIN `lod_sphere`. The projection divides by the distance to the
//      sphere's near point, so a parent sphere that did not contain its children's could project
//      to a SMALLER screen error than a child's despite a larger world error, and the two tests
//      would disagree about the ordering they are supposed to bracket.
//
// ================================================================================================
// WHY THE ERROR IS PROJECTED THROUGH A SPHERE AND NOT THROUGH THE CLUSTER'S BOUNDS
// ================================================================================================
//
// The bounds are the cluster's own and differ between the members of a group; the error is the
// group's. Projecting a shared number through an unshared volume would give the members of one
// group different answers to a test whose whole purpose is that they agree.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>

namespace cy::rendering::vg {

/// `virtual-geometry` — "Deformation classes and growth path". The table's five, in its order.
/// Static and RigidInstanced are required at M7; Terrain is a geometry source; the last two are
/// deferred and their seams are reserved rather than implemented.
enum class DeformationClass : u8 {
    Static = 0,
    RigidInstanced,
    Terrain,
    Destructible,
    Skinned,
    Count,
};

/// `virtual-geometry` — "Aggregate and thin geometry classification". Simplification and occlusion
/// behave differently for each, so the cooker branches on it rather than treating every triangle
/// identically.
enum class SurfaceClass : u8 {
    Solid = 0,
    Aggregate,
    Foliage,
    Hair,
    Thin,
    Count,
};

/// `virtual-geometry` — "Tangent policy". Declared per asset; the cooker defaults to `Derived` and
/// reports the storage saved.
enum class TangentPolicy : u8 {
    Stored = 0,
    Derived,
    Absent,
    Count,
};

/// `virtual-geometry` — "Geometry budget and importance": "Objects SHALL declare an importance —
/// critical, gameplay, normal, background — scaling their effective threshold".
enum class Importance : u8 {
    Critical = 0,
    Gameplay,
    Normal,
    Background,
    Count,
};

[[nodiscard]] const char* deformation_class_name(DeformationClass value) noexcept;
[[nodiscard]] const char* surface_class_name(SurfaceClass value) noexcept;
[[nodiscard]] const char* tangent_policy_name(TangentPolicy value) noexcept;
[[nodiscard]] const char* importance_name(Importance value) noexcept;

/// The threshold multiplier an importance applies. Below one keeps detail, above one gives it up
/// first — so background geometry coarsens before gameplay-critical geometry does, which is the
/// scenario the requirement names.
[[nodiscard]] f32 importance_threshold_scale(Importance value) noexcept;

/// `virtual-geometry` — "Cluster size SHALL be a **cooker policy** with configurable minimum,
/// target, and maximum triangle counts and a maximum vertex count — not a constant baked into the
/// format".
///
/// It is therefore recorded IN the cooked asset and read back out of it. A recook with a different
/// target changes no format version; the asset simply says a different thing about itself, which is
/// what "existing assets SHALL be recookable without a format version change" requires.
struct ClusterPolicy {
    u32 min_triangles = 32;
    u32 target_triangles = 128;
    u32 max_triangles = 128;
    /// At most 256, because a cluster's indices are stored as bytes. A larger cluster would need a
    /// wider index and would change the page format, which is why this is validated rather than
    /// clamped.
    u32 max_vertices = 192;
    /// How many clusters are collected into one group before the group is simplified as a unit.
    /// Nanite's range is 8 to 32; 8 is the default here because a group must be small enough that
    /// its interior is large relative to its locked boundary, and large enough that the boundary is
    /// a small fraction of it.
    u32 group_size = 8;
    /// The fraction of a group's triangles that survive simplification. One half is the classic
    /// choice: it halves the level count's base and keeps the error per level small enough that the
    /// hierarchy is deep rather than lossy.
    f32 simplify_ratio = 0.5F;
    /// Stop building levels when the whole mesh fits in this many clusters. One would be ideal and
    /// is not always reachable: a group of one cluster cannot be simplified below itself without
    /// unlocking its boundary.
    u32 root_cluster_limit = 2;
    /// A ceiling on the hierarchy, so a pathological mesh cannot loop. Reported when it is hit.
    u32 max_levels = 24;

    static constexpr u32 kVertexCeiling = 256;

    /// Fails naming the field and the limit. Called by the builder before anything is allocated.
    [[nodiscard]] Status validate() const noexcept;
};

/// `virtual-geometry` — "a **normal cone** (average normal and maximum deviation)". Stored as an
/// axis and the cosine of the half-angle, because the backface test is a dot product against the
/// cosine and storing the angle would mean a trigonometric call per cluster per frame.
///
/// `cos_angle` of -1 means "no useful cone": a cluster whose normals span more than a hemisphere
/// can never be rejected and says so, rather than being rejected wrongly.
struct NormalCone {
    Vec3 axis{0.0F, 0.0F, 1.0F};
    f32 cos_angle = -1.0F;

    [[nodiscard]] bool degenerate() const noexcept { return cos_angle <= -1.0F; }
};

/// A bounding sphere. `Sphere` in cy/core/math is centre-and-radius already; this alias exists so
/// that the two spheres below read as what they are rather than as two more shapes.
struct ErrorSphere {
    Vec3 center{0.0F, 0.0F, 0.0F};
    f32 radius = 0.0F;

    /// Whether `inner` is entirely inside this sphere. The monotonicity invariant the header
    /// comment describes, as a predicate the builder asserts and the test checks.
    [[nodiscard]] bool contains(const ErrorSphere& inner) const noexcept;
};

/// The error at which a hierarchy root stops: nothing is coarser, so the upper test never passes.
inline constexpr f32 kRootError = 3.402823466e+38F;

inline constexpr u32 kInvalidCluster = 0xFFFFFFFFU;
inline constexpr u32 kInvalidPage = 0xFFFFFFFFU;
inline constexpr u32 kInvalidGroup = 0xFFFFFFFFU;

/// One cluster, as the CPU sees it. The GPU record is `GpuCluster` in gpu_types.h and is a packed
/// projection of this; the two are kept in step by a static assertion and by a test that encodes
/// this and decodes that.
struct Cluster {
    /// Culling bounds, in the asset's own space.
    Aabb bounds;
    NormalCone cone;

    /// The group this cluster was produced from, and the sphere its error is projected through.
    /// Zero and the cluster's own sphere at level 0, where the geometry is the source geometry and
    /// deviates from it by nothing.
    f32 lod_error = 0.0F;
    ErrorSphere lod_sphere;
    /// The group this cluster was simplified into. `kRootError` when nothing coarser exists.
    f32 parent_error = kRootError;
    ErrorSphere parent_sphere;

    /// Where this cluster's triangles are in the asset's index array, and which vertices they use.
    u32 first_index = 0;
    u32 index_count = 0;
    u32 first_vertex = 0;
    u32 vertex_count = 0;

    /// The cluster's own UV range. `virtual-geometry` — "Geometry compression": "**UVs** quantised
    /// relative to a per-cluster range". Per cluster rather than per page for the same reason
    /// positions are: a cluster's encoded bytes then depend on that cluster's geometry alone, which
    /// is what makes the cook cache-friendly at cluster granularity (task 7.5). Degenerate — min
    /// above max — when the asset has no UVs.
    Vec2 uv_min{0.0F, 0.0F};
    Vec2 uv_max{0.0F, 0.0F};

    /// `virtual-geometry`: "its material range". A cluster is single-material by construction —
    /// clustering never mixes materials — so a range is a value here and the field is named for
    /// what the requirement calls it.
    u32 material = 0;

    /// Hierarchy links and streaming. `page` is the page holding this cluster's geometry;
    /// `first_child` / `child_count` index the asset's child table.
    u32 page = kInvalidPage;
    u32 group = kInvalidGroup;
    u32 first_child = 0;
    u32 child_count = 0;
    u8 level = 0;

    [[nodiscard]] u32 triangle_count() const noexcept { return index_count / 3U; }
};

/// The screen-space projection of a world-space error through a sphere.
///
/// `virtual-geometry` — "At runtime the error SHALL be converted to a **screen-space error** from
/// the node's bounds, the view projection, and the distance to the camera". The projection is the
/// standard one: a length `e` at distance `d` subtends `e * (h / 2) / (d * tan(fov/2))` pixels, and
/// `d` is measured to the NEAR POINT of the sphere so that a camera inside the sphere saturates
/// rather than dividing by a negative number.
struct ProjectionView {
    Vec3 camera_position{0.0F, 0.0F, 0.0F};
    /// Pixels of viewport height. The horizontal axis follows from the aspect ratio and does not
    /// enter: the error is isotropic and the vertical field of view is the one the projection
    /// fixes.
    f32 viewport_height = 1080.0F;
    f32 fov_y_radians = 1.0471975512F;
    /// Orthographic views have no distance term; `ortho_height` replaces the whole projection.
    bool orthographic = false;
    f32 ortho_height = 10.0F;

    /// Pixels per world unit at the sphere's near point. Cached by a traversal that projects many
    /// errors through one view.
    [[nodiscard]] f32 pixels_per_unit_at(const ErrorSphere& sphere) const noexcept;
};

/// The projected error, in pixels. Saturates at `kRootError` for the root, which is what keeps the
/// root's upper test true at every threshold without a special case in the traversal.
[[nodiscard]] f32 project_error(const ProjectionView& view, f32 error,
                                const ErrorSphere& sphere) noexcept;

/// The selection test, written once so that the CPU reference, the cook-time watertightness check
/// and the GPU shader cannot drift. The shader repeats it in Slang and `test_traversal.cpp`
/// compares the two buffer for buffer.
[[nodiscard]] bool cluster_selected(const Cluster& cluster, const ProjectionView& view,
                                    f32 threshold_pixels) noexcept;

/// Whether traversal should descend past this cluster: its own representation is too coarse.
[[nodiscard]] bool cluster_too_coarse(const Cluster& cluster, const ProjectionView& view,
                                      f32 threshold_pixels) noexcept;

/// `virtual-geometry` — "Normal cone rejects cheaply": true when the cone faces entirely away from
/// the camera and the cluster cannot contribute a front face.
[[nodiscard]] bool cone_backfacing(const NormalCone& cone, Vec3 cone_apex,
                                   Vec3 camera_position) noexcept;

}  // namespace cy::rendering::vg
