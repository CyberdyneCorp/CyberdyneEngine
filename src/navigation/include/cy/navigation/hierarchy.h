#pragma once
// Hierarchical pathfinding: a graph of regions above the polygon graph, planned over first and
// refined locally as an agent advances. M8.b task 6.1.
//
// ================================================================================================
// WHAT A REGION IS, AND WHY IT SURVIVES ITS TILE
// ================================================================================================
//
// A region is one connected component of one tile's polygons. That definition is chosen rather than
// inherited: `navigation` requires the hierarchy to be "built INCREMENTALLY as navigation data is
// generated or streamed" and "invalidated REGIONALLY when the underlying navigation changes", and a
// component of a tile is the largest unit both of those can be cheap for — rebuilding one tile
// recomputes that tile's components and nothing else's.
//
// A REGION OUTLIVES THE TILE IT WAS BUILT FROM. `release_tile()` marks its regions non-resident and
// keeps their identity, their bounds, their centres and every abstract edge they carry. That is
// what makes `navigation`'s "Paths crossing into unloaded regions SHALL be resolvable at the
// abstract level, with local refinement deferred until the region is resident" implementable at
// all: `plan()` still crosses the region and answers `complete = false`, and `refine()` on that
// segment refuses with `Unavailable` rather than dereferencing a tile that is gone.
//
// `forget_tile()` is the other operation and it is deliberately separate: a region that is gone for
// good is not the same fact as a region that is out of memory, and a caller that confused them
// would either leak abstract nodes forever or lose the ability to path across streamed-out content.
//
// ================================================================================================
// THE ABSTRACT SEARCH IS A* AND ITS TIE-BREAK IS THE REGION ID
// ================================================================================================
//
// The same rule query.h states for the polygon graph, for the same reason: two regions that reach
// the goal at equal cost must be ordered by something stable across runs, allocations and iteration
// orders, or one scenario produces two different plans.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/navigation/query.h>

namespace cy::navigation {

using RegionId = u32;
inline constexpr RegionId kInvalidRegion = 0xFFFFFFFFu;

/// One connected component of one tile.
struct NavRegion {
    TileCoord coord;
    /// Which component of that tile, in the order the flood fill found them — which is polygon
    /// order, which is the cooked tile's own order. Stable for a given tile.
    u32 component = 0;
    Vec3 centre;
    Aabb bounds;
    u32 polys = 0;
    /// False once `release_tile()` has run and until the tile is published again.
    bool resident = false;
};

/// One abstract connection: two regions that touch, across a tile border or through an off-mesh
/// link, and what crossing between their centres costs.
struct RegionEdge {
    RegionId to = kInvalidRegion;
    f32 cost = 0.0F;
};

/// A plan over the abstract graph.
struct AbstractPath {
    /// The regions to cross, start first.
    Array<RegionId> regions;
    f32 cost = 0.0F;
    bool found = false;
    /// True when every region on the path is resident, so every segment can be refined now. False
    /// is not a failure: it is `navigation`'s deferred refinement, and `first_unresident` says
    /// where the deferral starts.
    bool complete = false;
    u32 first_unresident = 0;

    explicit AbstractPath(Allocator& allocator) noexcept : regions(allocator) {}

    AbstractPath(const AbstractPath&) = delete;
    AbstractPath& operator=(const AbstractPath&) = delete;
    AbstractPath(AbstractPath&&) noexcept = default;
    AbstractPath& operator=(AbstractPath&&) noexcept = default;
};

/// What a rebuild cost, so "incremental" is a measurement.
struct HierarchyUpdate {
    u32 regions_added = 0;
    u32 regions_removed = 0;
    u32 edges_added = 0;
    u32 edges_removed = 0;
    u32 polys_visited = 0;
};

/// The region graph above one `NavMesh`.
///
/// It holds no reference to the mesh: every operation takes it, because a hierarchy that cached a
/// mesh pointer would be one more thing to invalidate when a tile is republished.
class NavHierarchy {
public:
    explicit NavHierarchy(Allocator& allocator) noexcept;

    NavHierarchy(const NavHierarchy&) = delete;
    NavHierarchy& operator=(const NavHierarchy&) = delete;
    NavHierarchy(NavHierarchy&&) noexcept = default;
    NavHierarchy& operator=(NavHierarchy&&) noexcept = default;

    /// Recompute one tile's regions and every abstract edge touching them. Idempotent, and the only
    /// way a region is created.
    [[nodiscard]] Expected<HierarchyUpdate, Error> rebuild_tile(const NavMesh& mesh,
                                                                TileCoord coord) noexcept;
    /// The tile streamed out. Its regions stay, marked non-resident; its edges stay.
    [[nodiscard]] Status release_tile(TileCoord coord) noexcept;
    /// The tile is gone for good. Its regions and every edge into them are dropped.
    [[nodiscard]] Status forget_tile(TileCoord coord) noexcept;

    [[nodiscard]] u32 region_count() const noexcept;
    [[nodiscard]] u32 live_region_count() const noexcept { return live_regions_; }
    [[nodiscard]] const NavRegion* region(RegionId id) const noexcept;
    /// The region a polygon belongs to, or `kInvalidRegion` when the tile is not in the hierarchy.
    [[nodiscard]] RegionId region_of(PolyRef ref, const NavMesh& mesh) const noexcept;
    [[nodiscard]] RegionId region_at(const NavMesh& mesh, Vec3 position,
                                     Vec3 extents) const noexcept;
    [[nodiscard]] Span<const RegionEdge> edges_of(RegionId id) const noexcept;

    /// A* over the abstract graph. `navigation`: "the search SHALL run over the abstract region
    /// graph and refine locally, rather than searching the full polygon graph".
    [[nodiscard]] Expected<AbstractPath, Error> plan(RegionId from, RegionId to) const noexcept;

    /// Refine one leg of an abstract plan into a polygon corridor.
    ///
    /// `segment` is an index into `path.regions`; the corridor runs from `from` to the centre of
    /// the next region, or to `goal` on the last segment. `navigation`: "subsequent segments SHALL
    /// be refined as needed, spreading cost over time".
    [[nodiscard]] Expected<PathResult, Error> refine(const NavMesh& mesh, const AbstractPath& path,
                                                     u32 segment, Vec3 from, Vec3 goal,
                                                     const PathFilter& filter,
                                                     PathCorridor& corridor) const noexcept;

    struct Stats {
        u32 regions = 0;
        u32 resident_regions = 0;
        u32 edges = 0;
        u32 rebuilds = 0;
    };
    [[nodiscard]] Stats stats() const noexcept;

private:
    struct RegionEntry {
        NavRegion region;
        Array<RegionEdge> edges;
        bool live = false;

        explicit RegionEntry(Allocator& allocator) noexcept : edges(allocator) {}
    };

    struct TileEntry {
        TileCoord coord;
        Array<RegionId> regions;
        /// One entry per polygon of the tile, naming the region it landed in. Rebuilt with the
        /// tile; the reason it exists is `region_of`, which every plan starts with.
        Array<RegionId> region_of_poly;
        u32 salt = 0;
        bool live = false;

        explicit TileEntry(Allocator& allocator) noexcept
            : regions(allocator), region_of_poly(allocator) {}
    };

    [[nodiscard]] u32 tile_entry(TileCoord coord) const noexcept;
    [[nodiscard]] Expected<u32, Error> acquire_tile(TileCoord coord) noexcept;
    [[nodiscard]] Expected<RegionId, Error> acquire_region() noexcept;
    void drop_regions_of(u32 entry, HierarchyUpdate& update) noexcept;
    [[nodiscard]] Status connect(RegionId from, RegionId to, f32 cost,
                                 HierarchyUpdate& update) noexcept;
    /// The two halves of `rebuild_tile`, and the two halves of `plan`. Split because each was well
    /// past the complexity this project holds systems code to, and because each is separately the
    /// subject of a case in `unit.navigation`.
    [[nodiscard]] Expected<RegionId, Error> flood_region(const NavMesh& mesh, u32 slot, u32 entry,
                                                         u32 seed, u32 component,
                                                         HierarchyUpdate& update) noexcept;
    [[nodiscard]] Status connect_tile_edges(const NavMesh& mesh, u32 slot, u32 entry,
                                            HierarchyUpdate& update) noexcept;
    [[nodiscard]] RegionId cheapest_open(Span<const f32> best, Span<const u8> open,
                                         Vec3 goal) const noexcept;
    [[nodiscard]] bool search(RegionId from, RegionId to, Array<f32>& best,
                              Array<RegionId>& came_from, Array<u8>& open) const noexcept;

    Array<RegionEntry> regions_;
    Array<TileEntry> tiles_;
    /// Scratch used by `rebuild_tile`'s flood fill. A member so a rebuild of a large tile does not
    /// allocate on every call.
    Array<u32> queue_;
    u32 live_regions_ = 0;
    u32 rebuilds_ = 0;
};

}  // namespace cy::navigation
