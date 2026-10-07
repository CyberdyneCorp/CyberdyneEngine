// SPDX-License-Identifier: MIT
#pragma once
// The navigation mesh of a `Fixed` world: the baked mesh converted ONCE, at load, before tick 0.
// openspec/changes/add-deterministic-math, design §9.1 and task 6.2.
//
// ================================================================================================
// WHY A CONVERTED COPY, AND WHAT IT KEEPS
// ================================================================================================
//
// Recast bakes in float, offline, and the `.cynavmesh` bytes are identical on every peer because
// the content manifest hash is part of the compatibility scope. What is NOT identical across
// architectures is any float arithmetic done on those bytes per tick. So a `Lockstep` world
// converts the vertices with the cooked conversion (`detmath::from_f32_cooked`, design §7.1) when
// it loads, and every query after that is integer arithmetic on the converted copy:
//
//   * the polygons, in the source mesh's (tile slot, polygon) order, which is the order of their
//     `PolyRef` bits and so the tie-break `navigation` already uses;
//   * the adjacency across every shared edge, within a tile and across a border;
//   * each polygon's area — as the source's obstacles left it at conversion — and its cost.
//
// The plane is (x, z): a `FixedVec2`'s `y` is world Z. A converted world is a single walkable
// layer, which is what an RTS map is. A polygon keeps its mean corner height; the mover takes a
// unit's height from cooked height samples (height_field.h) when the world has them.
//
// ================================================================================================
// WHAT A FIXED WORLD REFUSES
// ================================================================================================
//
// A runtime Recast rebuild is float work on each peer, so it is refused as authoritative input
// (design §9.1): the conversion records the source mesh's version, and `check_source()` refuses a
// mesh that has been republished since. A dynamic obstacle in a `Fixed` world is an integer
// per-polygon flag instead, set through a command: `set_blocked()`.
//
// Off-mesh links are not converted: a `Fixed` world that needs them declares navigation
// `SamePlatform` until they are, and the refusal names it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/detmath/fixed.h>
#include <cy/core/detmath/vec.h>
#include <cy/core/memory/array.h>
#include <cy/navigation/navmesh.h>

namespace cy::movement {

using detmath::Fixed;
using detmath::FixedVec2;
using detmath::WideFixed;

/// The index of a polygon in a `FixedNavMesh`.
using FixedPolyIndex = u32;
/// No polygon: off the mesh, a border edge, or not yet located.
inline constexpr FixedPolyIndex kNoPoly = 0xFFFF'FFFFU;

/// One converted polygon. Corners are a run of `FixedNavMesh::corners()` and of
/// `FixedNavMesh::neighbours()`: edge `i` runs from corner `i` to corner `i + 1` modulo the count,
/// and `neighbours()[first + i]` is the polygon across it, or `kNoPoly` at a border.
struct FixedNavPoly {
    u32 first = 0;
    u8 corner_count = 0;
    navigation::AreaType area = navigation::kAreaGround;
    /// The traversal cost per metre, converted from the source's float.
    Fixed cost = Fixed::one();
    /// The mean of the corners, each component truncated.
    FixedVec2 centre;
    /// The bounds of the corners on the plane.
    FixedVec2 min;
    FixedVec2 max;
    /// The mean corner height (world Y).
    Fixed height;
    /// The polygon this was converted from, for a diagnostic. Not used by any query.
    navigation::PolyRef source;
};

/// What a conversion saw, so a load can report rather than guess.
struct NavConversionReport {
    u32 tiles = 0;
    u32 polys = 0;
    u32 corners = 0;
    u32 internal_edges = 0;  ///< edges with a neighbour, counted once per side
    u32 border_edges = 0;
    u32 links_dropped = 0;  ///< off-mesh links, which a Fixed world does not traverse
};

/// A navigation mesh in `Fixed`: the baked mesh's polygons and adjacency converted once, at load,
/// so every query a `Lockstep` world makes is integer arithmetic.
class FixedNavMesh {
public:
    /// The bucket grid `locate()` uses is 2^kBucketShift metres on a side.
    static constexpr int kBucketShift = 3;

    explicit FixedNavMesh(Allocator& allocator) noexcept;

    FixedNavMesh(const FixedNavMesh&) = delete;
    FixedNavMesh& operator=(const FixedNavMesh&) = delete;
    FixedNavMesh(FixedNavMesh&&) noexcept = default;
    FixedNavMesh& operator=(FixedNavMesh&&) noexcept = default;

    /// Convert `source`. THE ONE FLOAT STEP: every vertex through `detmath::from_f32_cooked`, once.
    /// Refuses an empty mesh and a polygon whose corners do not convert inside the range.
    [[nodiscard]] Expected<NavConversionReport, Error> convert(
        const navigation::NavMesh& source) noexcept;

    /// Refuses a source that was republished after the conversion: a runtime rebuild is float work
    /// on each peer and is not authoritative input in a `Fixed` world.
    [[nodiscard]] Status check_source(const navigation::NavMesh& source) const noexcept;

    [[nodiscard]] u32 poly_count() const noexcept { return static_cast<u32>(polys_.size()); }
    [[nodiscard]] const FixedNavPoly& poly(FixedPolyIndex index) const noexcept {
        return polys_[index];
    }
    [[nodiscard]] Span<const FixedVec2> corners_of(FixedPolyIndex index) const noexcept {
        const FixedNavPoly& p = polys_[index];
        return {corners_.data() + p.first, p.corner_count};
    }
    [[nodiscard]] Span<const FixedPolyIndex> neighbours_of(FixedPolyIndex index) const noexcept {
        const FixedNavPoly& p = polys_[index];
        return {neighbours_.data() + p.first, p.corner_count};
    }
    /// For each edge of polygon `index`, the distance from its centre to the centre of the polygon
    /// across (correctly rounded), or zero at a border. Computed once at conversion: it is A*'s
    /// step length.
    [[nodiscard]] Span<const Fixed> crossings_of(FixedPolyIndex index) const noexcept {
        const FixedNavPoly& p = polys_[index];
        return {crossings_.data() + p.first, p.corner_count};
    }
    /// The version of the source mesh at conversion.
    [[nodiscard]] u32 source_version() const noexcept { return source_version_; }

    /// Mark a polygon impassable to path queries, or clear it. The integer obstacle of a `Fixed`
    /// world (design §9.1). Units already on it are still clamped to it.
    void set_blocked(FixedPolyIndex index, bool blocked) noexcept;
    [[nodiscard]] bool blocked(FixedPolyIndex index) const noexcept { return blocked_[index] != 0; }

    /// Whether `point` lies inside or on the boundary of polygon `index`. Decided on exact Q64.64
    /// cross products, so every peer agrees on a point exactly on an edge.
    [[nodiscard]] bool contains(FixedPolyIndex index, FixedVec2 point) const noexcept;

    /// The lowest-index polygon containing `point`, or `kNoPoly`. A point on a shared edge is in
    /// both polygons and answers the lower index, on every peer.
    [[nodiscard]] FixedPolyIndex locate(FixedVec2 point) const noexcept;

    /// The point of polygon `index` nearest `point`: `point` itself when inside, otherwise the
    /// nearest point of its boundary, with ties going to the lower edge.
    [[nodiscard]] FixedVec2 closest_point(FixedPolyIndex index, FixedVec2 point) const noexcept;

    /// The polygon nearest `point` over the whole mesh, and the point on it. Ties go to the lower
    /// index. What a unit that is off the mesh, or an order aimed off it, is snapped with.
    [[nodiscard]] FixedPolyIndex nearest(FixedVec2 point, FixedVec2& on_mesh) const noexcept;

    /// Move from polygon `from` toward `desired`, staying on the mesh: `desired` when it lies in
    /// `from` or in one of its neighbours, otherwise the nearest point of their boundaries. `from`
    /// is updated to the polygon the answer lies in. This is the mover's clamp to the navigation
    /// surface (design §8); one ring of neighbours suffices for a unit that crosses less than a
    /// polygon per tick, and a faster one is stopped at the ring's edge rather than tunnelling.
    [[nodiscard]] FixedVec2 clamp_move(FixedPolyIndex& from, FixedVec2 desired) const noexcept;

    /// The shared edge between two adjacent polygons, as stored in `from` (corner i, corner i+1).
    /// False when they are not adjacent.
    [[nodiscard]] bool portal(FixedPolyIndex from, FixedPolyIndex to, FixedVec2& a,
                              FixedVec2& b) const noexcept;

    /// The plane bounds of the whole mesh.
    [[nodiscard]] FixedVec2 min() const noexcept { return min_; }
    [[nodiscard]] FixedVec2 max() const noexcept { return max_; }

    /// A digest of the converted mesh: every corner, adjacency, area and cost, in order. Folded
    /// into a session's state at tick 0, so two peers that converted differently disagree before
    /// the first tick rather than after it.
    [[nodiscard]] u64 digest() const noexcept;

private:
    [[nodiscard]] Status build_buckets() noexcept;
    [[nodiscard]] Status build_crossings() noexcept;
    [[nodiscard]] i64 bucket_x(Fixed x) const noexcept;
    [[nodiscard]] i64 bucket_z(Fixed z) const noexcept;

    Array<FixedNavPoly> polys_;
    Array<FixedVec2> corners_;
    Array<Fixed> corner_heights_;
    Array<FixedPolyIndex> neighbours_;
    Array<Fixed> crossings_;
    Array<u8> blocked_;
    /// `bucket_starts_[b]` .. `bucket_starts_[b + 1]` index `bucket_polys_`, in polygon order.
    Array<u32> bucket_starts_;
    Array<FixedPolyIndex> bucket_polys_;
    FixedVec2 min_;
    FixedVec2 max_;
    i64 bucket_origin_x_ = 0;
    i64 bucket_origin_z_ = 0;
    u32 buckets_x_ = 0;
    u32 buckets_z_ = 0;
    u32 source_version_ = 0;
};

}  // namespace cy::movement
