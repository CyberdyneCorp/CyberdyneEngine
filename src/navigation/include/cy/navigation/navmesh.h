#pragma once
// The navigation mesh: convex polygons with adjacency, area types and traversal costs, organised
// into tiles that are rebuilt, published and released independently. M8.b task 6.1.
//
// ================================================================================================
// WHY A TILE OWNS ITS POLYGONS, AND WHY A REFERENCE CARRIES A SALT
// ================================================================================================
//
// `navigation` asks for three things that together fix this layout:
//
//   * "organised into TILES so regions can be rebuilt independently";
//   * "Rebuilds SHALL run on job workers and be PUBLISHED ATOMICALLY, so queries always see a
//     consistent mesh";
//   * "a region unloads while an agent is pathing through it ... the agent's path SHALL be
//     invalidated cleanly and a repath triggered, RATHER THAN DEREFERENCING RELEASED DATA".
//
// One flat polygon array across the whole mesh satisfies none of them: rebuilding one tile would
// renumber every polygon after it, and every corridor an agent held would silently come to mean a
// different polygon. So each tile owns its vertices, its polygons and its adjacency, a `PolyRef` is
// (tile, salt, polygon), and the salt advances every time a tile slot is reused. A reference into a
// released tile does not dereference anything: `NavMesh::poly()` answers null, and that is what
// makes "invalidated cleanly" a checkable property rather than a hope.
//
// ================================================================================================
// WHAT IS ENGINE-OWNED AND WHAT IS RECAST'S — the line, stated once
// ================================================================================================
//
// Recast voxelises source geometry and extracts walkable convex polygons. That is the step
// `ai-system` names as "not differentiating" and `navigation` describes procedurally (cell size,
// agent radius, region merge sizes, edge error, detail sample distance) — see build.h.
//
// EVERYTHING ABOVE THAT IS THIS ENGINE'S: the runtime representation here, the tile layout, cross
// tile adjacency, A* and the funnel (query.h), the region hierarchy (hierarchy.h), flow fields
// (flow_field.h), avoidance and crowds (crowd.h). Detour — Recast's own runtime half — is
// deliberately NOT integrated, and the reason is not taste: `navigation` requires navigation tiling
// to be independent of world cells, requires async queries to complete on a DETERMINISTIC tick, and
// requires flow fields and hierarchical refinement that Detour does not have. A runtime that owned
// its own allocation, its own tile identity and its own query scheduling would have to be worked
// around at each of those points, and a dependency worked around at three points is a dependency
// that costs more than it saves.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

namespace cy::navigation {

// --- Areas ---------------------------------------------------------------------------------------
//
// An area type is a small integer and a mask is 64 bits of them, so "exclude water and lava" is one
// AND rather than a set lookup per polygon expanded. Sixty-four is the same ceiling Recast's own
// area byte implies and is what `NavAreaCosts` below is sized for.

using AreaType = u8;
inline constexpr u32 kAreaCount = 64;
/// The default: ordinary ground.
inline constexpr AreaType kAreaGround = 0;
/// Recast's convention and this engine's: the area a polygon has when it is not walkable at all.
/// A polygon never carries it — an unwalkable surface produces no polygon — but a build parameter
/// and an obstacle both use it to mark geometry out.
inline constexpr AreaType kAreaNull = 63;

/// Which area types a query may traverse at all. `navigation`: "an area mask excluding types
/// entirely".
using AreaMask = u64;
inline constexpr AreaMask kAllAreas = ~AreaMask{0} & ~(AreaMask{1} << kAreaNull);

[[nodiscard]] constexpr AreaMask area_bit(AreaType area) noexcept {
    return AreaMask{1} << (area & (kAreaCount - 1));
}

/// Per-agent area cost multipliers. `navigation`: "so an agent can prefer roads or avoid water".
/// A multiplier below one is a preference and above one is an aversion; the traversal cost of a
/// polygon is its own cost times this.
struct NavAreaCosts {
    f32 multiplier[kAreaCount] = {};

    /// All ones. Written as a function rather than a default member initialiser because sixty-four
    /// of them in a brace list is not a thing anybody should read.
    [[nodiscard]] static NavAreaCosts uniform() noexcept;

    [[nodiscard]] f32 of(AreaType area) const noexcept {
        return multiplier[area & (kAreaCount - 1)];
    }
};

// --- Polygon references --------------------------------------------------------------------------

/// A reference to one polygon of one tile, valid only while that tile's salt is unchanged.
///
/// Twenty bits each: a million tiles, a million polygons in a tile, and a salt that wraps after a
/// million reuses of one slot. The salt is what makes a stale reference detectable rather than
/// dangerous — see the header comment.
class PolyRef {
public:
    constexpr PolyRef() noexcept = default;

    [[nodiscard]] static constexpr PolyRef make(u32 tile, u32 salt, u32 poly) noexcept {
        return PolyRef((u64{tile & kMask} << 40) | (u64{salt & kMask} << 20) | u64{poly & kMask} |
                       kValidBit);
    }

    [[nodiscard]] constexpr bool valid() const noexcept { return (bits_ & kValidBit) != 0; }
    [[nodiscard]] constexpr u32 tile() const noexcept {
        return static_cast<u32>((bits_ >> 40) & kMask);
    }
    [[nodiscard]] constexpr u32 salt() const noexcept {
        return static_cast<u32>((bits_ >> 20) & kMask);
    }
    [[nodiscard]] constexpr u32 poly() const noexcept { return static_cast<u32>(bits_ & kMask); }
    [[nodiscard]] constexpr u64 bits() const noexcept { return bits_; }

    friend constexpr bool operator==(PolyRef a, PolyRef b) noexcept { return a.bits_ == b.bits_; }
    friend constexpr bool operator!=(PolyRef a, PolyRef b) noexcept { return a.bits_ != b.bits_; }
    friend constexpr bool operator<(PolyRef a, PolyRef b) noexcept { return a.bits_ < b.bits_; }

private:
    explicit constexpr PolyRef(u64 bits) noexcept : bits_(bits) {}

    static constexpr u64 kMask = 0xFFFFFu;
    static constexpr u64 kValidBit = u64{1} << 63;

    u64 bits_ = 0;
};

inline constexpr PolyRef kInvalidPoly{};

/// The longest polygon this engine builds. Recast's own default is six and the funnel walks the
/// corners of every polygon it crosses, so a bound here keeps a corridor step's cost fixed.
inline constexpr u32 kMaxPolyVertices = 6;

/// One convex polygon. Vertices and neighbours are parallel runs in the owning tile's arrays: edge
/// `i` of the polygon runs from vertex `i` to vertex `i + 1` modulo the count, and `neighbours[i]`
/// is what lies across it.
struct NavPoly {
    u32 first_corner = 0;  ///< into NavTile::corners() and NavTile::neighbours()
    u8 corner_count = 0;
    AreaType area = kAreaGround;
    /// The polygon's own traversal cost per metre, before the agent's area multiplier.
    f32 cost = 1.0f;
    Vec3 centre;
};

/// Where a tile sits. `navigation` requires navigation to keep "its own tile layout" whose
/// "boundaries SHALL NOT be required to match world cell boundaries", so a coordinate here is in
/// units of `NavMesh::tile_size()` and is unrelated to any world cell coordinate.
struct TileCoord {
    i32 x = 0;
    i32 z = 0;
    /// Floors of a building, decks of a ship: two tiles at one (x, z) that do not touch.
    i32 layer = 0;

    friend constexpr bool operator==(TileCoord a, TileCoord b) noexcept {
        return a.x == b.x && a.z == b.z && a.layer == b.layer;
    }
    friend constexpr bool operator!=(TileCoord a, TileCoord b) noexcept { return !(a == b); }
};

/// The polygons of one tile, as `build_tile()` produces them and `NavMesh::add_tile()` consumes
/// them. A plain value: it is produced on a job worker and published on the thread that owns the
/// mesh, and nothing in it points at the mesh.
class NavTileData {
public:
    explicit NavTileData(Allocator& allocator) noexcept
        : vertices_(allocator), polys_(allocator), corners_(allocator) {}

    NavTileData(const NavTileData&) = delete;
    NavTileData& operator=(const NavTileData&) = delete;
    NavTileData(NavTileData&&) noexcept = default;
    NavTileData& operator=(NavTileData&&) noexcept = default;

    TileCoord coord;
    Aabb bounds;

    [[nodiscard]] Array<Vec3>& vertices() noexcept { return vertices_; }
    [[nodiscard]] Span<const Vec3> vertices() const noexcept { return vertices_.span(); }
    [[nodiscard]] Array<NavPoly>& polys() noexcept { return polys_; }
    [[nodiscard]] Span<const NavPoly> polys() const noexcept { return polys_.span(); }
    /// Vertex indices, `corner_count` per polygon, in winding order.
    [[nodiscard]] Array<u32>& corners() noexcept { return corners_; }
    [[nodiscard]] Span<const u32> corners() const noexcept { return corners_.span(); }

    /// Recompute `bounds` and every polygon's centre from the vertices. Called by `add_tile`, so a
    /// producer that forgot is corrected rather than trusted.
    void finalise() noexcept;

private:
    Array<Vec3> vertices_;
    Array<NavPoly> polys_;
    Array<u32> corners_;
};

// --- Off-mesh links
// --------------------------------------------------------------------------------
//
// `navigation`: "explicit connections between two points that are not traversable through the mesh,
// such as jumps, ladders, doors, and teleporters", carrying "bidirectionality, a cost, an area
// type, an agent-capability requirement, and a user-defined action tag that gameplay interprets".

/// What an agent can do. A link an agent lacks the capability for is excluded from its search.
using CapabilityMask = u64;
inline constexpr CapabilityMask kNoCapabilities = 0;
inline constexpr CapabilityMask kAllCapabilities = ~CapabilityMask{0};

using LinkId = u32;
inline constexpr LinkId kInvalidLink = 0xFFFFFFFFu;

struct NavLink {
    Vec3 from;
    Vec3 to;
    PolyRef from_poly;
    PolyRef to_poly;
    f32 cost = 1.0f;
    AreaType area = kAreaGround;
    /// Every bit set here must be set on the agent, or the link is not expanded.
    CapabilityMask requires_capabilities = kNoCapabilities;
    /// Gameplay's own meaning: `Jump`, `Ladder`, `Door`. Navigation never interprets it.
    Name action;
    bool bidirectional = true;
};

// --- Obstacles
// ---------------------------------------------------------------------------------------
//
// `navigation`: "navigation obstacles that carve or mark areas WITHOUT A FULL REBUILD", and its
// scenario: "a crate is dropped ... it SHALL mark its footprint as unwalkable through an obstacle
// rather than triggering a voxelisation rebuild".

using ObstacleId = u32;
inline constexpr ObstacleId kInvalidObstacle = 0xFFFFFFFFu;

struct NavObstacleShape {
    /// A vertical cylinder when `radius` is positive, otherwise the box `bounds`.
    Vec3 centre;
    f32 radius = 0.0f;
    f32 height = 0.0f;
    Aabb bounds;
    /// The area the footprint is marked with. `kAreaNull` carves it out entirely.
    AreaType area = kAreaNull;

    [[nodiscard]] bool contains(Vec3 point) const noexcept;
    [[nodiscard]] Aabb extent() const noexcept;
};

/// The agent the mesh was generated for. `navigation`: "a separate navigation mesh SHALL be
/// generated per agent profile, selectable per agent".
struct AgentProfile {
    Name name;
    f32 radius = 0.5f;
    f32 height = 2.0f;
    f32 max_climb = 0.4f;
    f32 max_slope_degrees = 45.0f;
};

/// What a tile publication changed, so a caller can report rather than guess.
struct TileChange {
    TileCoord coord;
    u32 tile = 0;
    u32 salt = 0;
    u32 polys = 0;
    u32 internal_edges = 0;
    u32 border_edges = 0;
    bool replaced_existing = false;
};

/// A navigation mesh: one agent profile's polygons, tiled.
///
/// NOT THREAD-SAFE FOR WRITING. `navigation` puts rebuilds on job workers and publication on one
/// thread — build a `NavTileData` anywhere, call `add_tile` from the thread that owns the mesh.
/// Queries are const and any number may run at once.
class NavMesh {
public:
    NavMesh(Allocator& allocator, Name name, f32 tile_size) noexcept;

    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;
    NavMesh(NavMesh&&) noexcept = default;
    NavMesh& operator=(NavMesh&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] f32 tile_size() const noexcept { return tile_size_; }
    [[nodiscard]] Allocator& allocator() const noexcept { return tiles_.allocator(); }
    [[nodiscard]] const AgentProfile& profile() const noexcept { return profile_; }
    void set_profile(const AgentProfile& profile) noexcept { profile_ = profile; }

    /// Advanced by every publication and every removal. `navigation`'s "the navigation mesh version
    /// changes and an agent's corridor is invalidated" is a comparison against this.
    [[nodiscard]] u32 version() const noexcept { return version_; }

    // --- Tiles
    // ------------------------------------------------------------------------------------

    /// Publish a tile, replacing whatever occupied its coordinate. Atomic from a query's point of
    /// view: the tile's polygons and every cross-tile edge into it are connected before the slot
    /// becomes visible, and the previous occupant's salt is retired first, so a corridor that named
    /// it stops resolving rather than resolving to something else.
    [[nodiscard]] Expected<TileChange, Error> add_tile(NavTileData&& data) noexcept;

    /// Release a tile. Its salt advances, so every outstanding reference into it becomes stale.
    [[nodiscard]] Status remove_tile(TileCoord coord) noexcept;

    [[nodiscard]] u32 tile_count() const noexcept { return live_tiles_; }
    [[nodiscard]] u32 tile_capacity() const noexcept { return static_cast<u32>(tiles_.size()); }
    [[nodiscard]] bool tile_resident(TileCoord coord) const noexcept;
    /// The slot index of a coordinate, or `0xFFFFFFFF`.
    [[nodiscard]] u32 tile_slot(TileCoord coord) const noexcept;
    [[nodiscard]] TileCoord tile_coord(u32 slot) const noexcept;
    [[nodiscard]] Aabb tile_bounds(u32 slot) const noexcept;
    /// The tile coordinate a world position falls in, at layer zero.
    [[nodiscard]] TileCoord coord_of(Vec3 position) const noexcept;

    // --- Polygons
    // ---------------------------------------------------------------------------------

    /// The polygon a reference names, or null when the reference is stale or the tile is gone.
    [[nodiscard]] const NavPoly* poly(PolyRef ref) const noexcept;
    [[nodiscard]] Span<const u32> poly_corners(PolyRef ref) const noexcept;
    [[nodiscard]] Span<const PolyRef> poly_neighbours(PolyRef ref) const noexcept;
    [[nodiscard]] Span<const Vec3> tile_vertices(u32 slot) const noexcept;
    /// Every polygon of one tile, as references. Empty when the slot is not resident.
    [[nodiscard]] u32 tile_poly_count(u32 slot) const noexcept;
    [[nodiscard]] PolyRef tile_poly(u32 slot, u32 index) const noexcept;

    /// The polygon's corner positions, in winding order, written into `out` (up to
    /// `kMaxPolyVertices`). Returns how many were written.
    [[nodiscard]] u32 poly_vertices(PolyRef ref, Vec3* out, u32 capacity) const noexcept;
    /// The shared edge between two adjacent polygons, as its two endpoints. False when they are not
    /// adjacent — which is what the funnel needs to refuse a corridor that does not connect.
    [[nodiscard]] bool portal(PolyRef from, PolyRef to, Vec3& left, Vec3& right) const noexcept;

    /// The polygon nearest `point` within `extents` of it, and the closest position on it.
    ///
    /// `navigation` does not name this operation, and every other one needs it: a query starts from
    /// a position and an agent is somewhere, not on a polygon reference.
    [[nodiscard]] PolyRef find_nearest(Vec3 point, Vec3 extents, AreaMask areas,
                                       Vec3& nearest) const noexcept;
    [[nodiscard]] bool contains_point(PolyRef ref, Vec3 point, f32 vertical_tolerance,
                                      Vec3& on_poly) const noexcept;

    // --- Obstacles
    // ---------------------------------------------------------------------------------

    /// Add an obstacle. Polygons whose centre it covers take its area immediately; nothing is
    /// voxelised and no tile is rebuilt. Returns its id.
    [[nodiscard]] Expected<ObstacleId, Error> add_obstacle(const NavObstacleShape& shape) noexcept;
    [[nodiscard]] Status remove_obstacle(ObstacleId id) noexcept;
    [[nodiscard]] u32 obstacle_count() const noexcept;
    /// The area a polygon presents to a query: its own, unless an obstacle marks it.
    [[nodiscard]] AreaType effective_area(PolyRef ref) const noexcept;

    // --- Off-mesh links
    // ------------------------------------------------------------------------------

    /// Connect two points. Both ends are snapped to the nearest polygon within `snap`; a link whose
    /// ends do not land on the mesh is refused rather than silently disconnected.
    [[nodiscard]] Expected<LinkId, Error> add_link(const NavLink& link, Vec3 snap) noexcept;
    [[nodiscard]] Status remove_link(LinkId id) noexcept;
    [[nodiscard]] const NavLink* link(LinkId id) const noexcept;
    [[nodiscard]] u32 link_count() const noexcept { return static_cast<u32>(links_.size()); }
    /// Links leaving `ref`, as ids. Both directions of a bidirectional link appear on both ends.
    [[nodiscard]] Span<const LinkId> links_from(PolyRef ref) const noexcept;

    // --- Statistics
    // ----------------------------------------------------------------------------------

    struct Stats {
        u32 tiles = 0;
        u32 polys = 0;
        u32 vertices = 0;
        u32 border_edges = 0;
        u32 links = 0;
        u32 obstacles = 0;
        u32 tile_builds = 0;
        u32 tile_removals = 0;
    };
    [[nodiscard]] Stats stats() const noexcept;

private:
    struct Tile {
        TileCoord coord;
        Aabb bounds;
        Array<Vec3> vertices;
        Array<NavPoly> polys;
        Array<u32> corners;
        Array<PolyRef> neighbours;
        Array<LinkId> links;
        /// Where each polygon's links start in `links`, `polys.size() + 1` entries.
        Array<u32> link_starts;
        /// The area an obstacle imposes, one per polygon, or `kAreaCount` for "none".
        Array<u8> obstacle_area;
        u32 salt = 1;
        bool resident = false;

        explicit Tile(Allocator& allocator) noexcept
            : vertices(allocator),
              polys(allocator),
              corners(allocator),
              neighbours(allocator),
              links(allocator),
              link_starts(allocator),
              obstacle_area(allocator) {}
    };

    struct ObstacleEntry {
        NavObstacleShape shape;
        ObstacleId id = kInvalidObstacle;
        bool live = false;
    };

    /// A link, plus what it needs to be re-snapped when the tile under one of its ends is rebuilt.
    /// Without the snap extent a rebuilt tile would leave every jump and ladder over it dangling,
    /// which is the failure `navigation`'s "door opens" scenario would produce on the second open.
    struct LinkEntry {
        NavLink link;
        Vec3 snap;
        bool live = false;
    };

    [[nodiscard]] const Tile* resident_tile(u32 slot) const noexcept;
    [[nodiscard]] Expected<u32, Error> acquire_slot(TileCoord coord) noexcept;
    void disconnect_tile(u32 slot) noexcept;
    [[nodiscard]] Status connect_tile(u32 slot, u32& internal_edges, u32& border_edges) noexcept;
    /// The two halves of `connect_tile`: adjacency inside one tile, by welded vertex index, and
    /// adjacency across a tile border, by position. Split because together they were the most
    /// complex function in this module by a wide margin, and because the two match on different
    /// things for different reasons.
    [[nodiscard]] u32 connect_within(u32 slot) noexcept;
    [[nodiscard]] bool connect_across(u32 slot, u32 other_slot, usize poly_index, usize here,
                                      Vec3 a0, Vec3 a1) noexcept;
    [[nodiscard]] Status rebuild_links(u32 slot) noexcept;
    void reapply_obstacles(u32 slot) noexcept;

    Name name_;
    f32 tile_size_ = 32.0f;
    AgentProfile profile_;
    Array<Tile> tiles_;
    Array<TileCoord> tile_coords_;  ///< parallel to tiles_, for the linear coordinate lookup
    Array<ObstacleEntry> obstacles_;
    Array<LinkEntry> links_;
    u32 version_ = 1;
    u32 live_tiles_ = 0;
    u32 tile_builds_ = 0;
    u32 tile_removals_ = 0;
};

}  // namespace cy::navigation
