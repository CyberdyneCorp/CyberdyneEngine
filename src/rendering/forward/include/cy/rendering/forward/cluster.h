#pragma once
// The cluster grid, and the assignment of lights and volumes to it. Task 4.3.1.
//
// `rendering-forward-clustered` — "Cluster grid": a 2D screen tiling (default 32×32 pixels)
// subdivided into depth slices (default 32) "distributed **exponentially** in view depth, so slices
// are thin near the camera and thick far away", with
//
//     slice = floor(log(z / near) · slice_count / log(far / near))
//
// and "the mapping constants supplied in a uniform buffer so the shader computes it with two
// instructions". Those two constants are `slice_scale` and `slice_bias` below, and
// `cy/cluster.slang` already reads them by those names — this struct is that one, field for field.
//
// ================================================================================================
// WHY THE ASSIGNMENT EXISTS ON THE CPU WHEN THE SPECIFICATION SAYS COMPUTE PASS
// ================================================================================================
//
// It says: "Assignment SHALL run as a **compute pass**". It does — `forward/frame.h` declares one,
// with its dispatch sized from this grid. What is HERE is the same algorithm written once in C++,
// and it earns its place three times over:
//
//   * it is the null backend's answer, so a frame's light lists exist in continuous integration on
//   a
//     machine with no GPU, which is what makes the rest of the frame testable there;
//   * it is the reference the compute pass is checked against — a divergence between the two is a
//     defect in one of them, and without a second implementation there is nothing to compare;
//   * every decision the specification actually pins down (the 60° spot threshold, the bounded
//     per-cluster count, deterministic nearest-kept dropping, the camera-inside-a-light case) is
//     stated once, here, in a form a test can assert on.
//
// The cost is real and is worth naming: two implementations of one algorithm can drift. The
// mitigation is that this one is the reference and the specification's numbers live in it.
//
// ================================================================================================
// EVERYTHING IS IN VIEW SPACE, AND THAT IS NOT AN IMPLEMENTATION DETAIL
// ================================================================================================
//
// A cluster's bounds are defined by a screen tile and a depth slice, both of which are view-space
// facts. Assigning in world space would mean transforming every cluster's bounds per frame instead
// of every element's position once — a hundred thousand transforms instead of a hundred — and it
// would make the depth slicing, which is a function of view depth, a function of something derived.
//
// View-space depth here is POSITIVE going away from the camera, even though the camera looks down
// its local −Z. That is the convention `cy/cluster.slang` computes in and the one the slice formula
// is written in; the negation happens once, in `view_depth_of()`, rather than at every call site.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

#include <cstddef>
#include <type_traits>

namespace cy::rendering {

/// The element types a cluster carries a list of. `rendering-forward-clustered` names lights,
/// decals and reflection probes; each gets its own `(offset, count)` header so a fragment iterating
/// lights does not walk decals.
enum class ClusterElementType : u8 {
    Light = 0,
    Decal,
    ReflectionProbe,
    Count,
};

inline constexpr u32 kClusterElementTypeCount = static_cast<u32>(ClusterElementType::Count);

/// The spot half-angle past which a cone bound is abandoned for a sphere.
///
/// `rendering-forward-clustered`: "Spot lights whose half-angle exceeds a threshold (**60°**) SHALL
/// be treated as spheres, because a cone bound becomes both loose and, past 90°, incorrect." The
/// number is the specification's; it is a constant here so a test can name it.
inline constexpr f32 kSpotConeThresholdRadians = 1.0471975512F;  // 60 degrees

/// THE GRID, IN THE LAYOUT cy/cluster.slang READS. 32 bytes.
struct ClusterGrid {
    u32 dimensions[3] = {0, 0, 0};
    u32 max_elements_per_cluster = 64;
    f32 near_plane = 0.1F;
    f32 far_plane = 1000.0F;
    /// `dimensions.z / log2(far / near)`.
    f32 slice_scale = 0.0F;
    /// `-dimensions.z · log2(near) / log2(far / near)`.
    f32 slice_bias = 0.0F;

    [[nodiscard]] constexpr u32 cluster_count() const noexcept {
        return dimensions[0] * dimensions[1] * dimensions[2];
    }
};

static_assert(sizeof(ClusterGrid) == 32, "ClusterGrid is cy/cluster.slang's ClusterGrid");
static_assert(offsetof(ClusterGrid, max_elements_per_cluster) == 12);
static_assert(offsetof(ClusterGrid, slice_scale) == 24);
static_assert(std::is_trivially_copyable_v<ClusterGrid>);

struct ClusterGridConfig {
    /// Screen tile, in pixels. The grid is resized so this stays constant across a resolution
    /// change, which is the "cluster dimensions adapt to resolution" scenario.
    u32 tile_size = 32;
    u32 slice_count = 32;
    u32 max_elements_per_cluster = 64;
};

/// Build a grid for a viewport and a depth range.
///
/// `far_plane` must be finite: the slice mapping is logarithmic in `far / near` and an infinite far
/// plane has no last slice. A view with an infinite projection passes its SHADOW distance or its
/// content's extent here, which is what the number is for — clusters exist to bound light lists,
/// not to cover everything the projection can represent.
[[nodiscard]] Expected<ClusterGrid, Error> make_cluster_grid(const ClusterGridConfig& config,
                                                             u32 view_width, u32 view_height,
                                                             f32 near_plane,
                                                             f32 far_plane) noexcept;

/// Positive view-space depth from a view-space position, whose z is negative in front of the
/// camera.
[[nodiscard]] constexpr f32 view_depth_of(Vec3 view_position) noexcept {
    return -view_position.z;
}

/// The depth slice a positive view depth falls in. The same two instructions `cy/cluster.slang`
/// computes, so the CPU reference and the shader cannot disagree about a boundary.
[[nodiscard]] u32 cluster_slice_of(const ClusterGrid& grid, f32 view_depth) noexcept;

/// The linear index of a cluster. Row-major over (x, y, slice) — one ordering, stated once, and the
/// one `clusterIndexOf` uses.
[[nodiscard]] constexpr u32 cluster_index_of(const ClusterGrid& grid, u32 x, u32 y,
                                             u32 slice) noexcept {
    return x + (grid.dimensions[0] * (y + (grid.dimensions[1] * slice)));
}

/// One cluster's view-space bounds. Derived from its tile and its slice, never stored per cluster:
/// sixty thousand boxes is a megabyte the assignment would have to read instead of computing six
/// multiplies.
[[nodiscard]] Aabb cluster_bounds(const ClusterGrid& grid, u32 x, u32 y, u32 slice,
                                  f32 tan_half_fov_y, f32 aspect) noexcept;

/// One thing to be assigned, in view space.
struct ClusterElement {
    /// The centre of the element's bounding sphere, in view space.
    Vec3 view_position{0.0F, 0.0F, 0.0F};
    f32 radius = 0.0F;
    /// The cone axis, in view space. Ignored unless `cone_cos` is below 1.
    Vec3 view_direction{0.0F, 0.0F, -1.0F};
    /// `cos(outer half-angle)`. 1 means "not a cone" — a point light, a probe, a wide spot past the
    /// 60° threshold. Whoever builds the element applies that threshold; `element_is_cone()` says
    /// what the rule is so two callers cannot apply it differently.
    f32 cone_cos = 1.0F;
    /// What the index buffer records. The caller's own index into its light or decal array — never
    /// this element's position in the input, so an input reordered for any reason still writes the
    /// same numbers.
    u32 payload_index = 0;
    u32 layer_mask = 0xFFFFFFFFU;
    ClusterElementType type = ClusterElementType::Light;
};

/// Whether a spot of this half-angle is bounded as a cone or as a sphere. See
/// `kSpotConeThresholdRadians`.
[[nodiscard]] bool element_is_cone(f32 outer_half_angle_radians) noexcept;

/// Per cluster, per type: where this cluster's list starts in the index buffer, and how long it is.
struct ClusterHeader {
    u32 offset = 0;
    u32 count = 0;
};

struct ClusterStatistics {
    u32 clusters = 0;
    u32 elements = 0;
    /// The two numbers `rendering-forward-clustered`'s diagnostics requirement names: "average and
    /// maximum elements per cluster, overflow count".
    f32 average_elements = 0.0F;
    u32 max_elements = 0;
    /// How many assignments were dropped because a cluster was full. "Overflow counter SHALL be
    /// reported in render statistics" — this is that counter.
    u32 overflow = 0;
    u32 non_empty_clusters = 0;
};

/// The result: the headers and the index buffer, in the layout a shader reads.
struct ClusterAssignment {
    explicit ClusterAssignment(Allocator& allocator) noexcept;

    ClusterAssignment(const ClusterAssignment&) = delete;
    ClusterAssignment& operator=(const ClusterAssignment&) = delete;

    /// `cluster_count × kClusterElementTypeCount` entries, indexed
    /// `cluster * kClusterElementTypeCount + type`.
    Array<ClusterHeader> headers;
    /// The concatenated lists. A cluster's list for a type is
    /// `indices[header.offset .. header.offset + header.count)`.
    Array<u32> indices;
    ClusterStatistics stats{};

    void clear() noexcept;
};

/// Assign elements to clusters. The CPU reference; see the header comment for why it exists.
///
/// `view_layer_mask` is the light-channel test: `rendering-lighting-and-shadows` requires channels
/// to be "a compact bitfield test during light assignment, not a per-object light loop", which is
/// this one AND — and it is why channels cost nothing per pixel.
///
/// Overflow drops the FURTHEST element from the cluster's centre, keeping the nearest, and the
/// surviving list is written in ascending payload order. Both halves matter: nearest-kept is what
/// the requirement says, and ascending order is what makes two runs produce identical buffers
/// however the drops fell out.
[[nodiscard]] Status assign_clusters(const ClusterGrid& grid, Span<const ClusterElement> elements,
                                     u32 view_layer_mask, f32 tan_half_fov_y, f32 aspect,
                                     ClusterAssignment& out) noexcept;

}  // namespace cy::rendering
