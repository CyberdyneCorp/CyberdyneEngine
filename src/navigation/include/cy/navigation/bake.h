// SPDX-License-Identifier: MIT
#pragma once
// Baking a navigation world: the tiling loop above `build_tile`, incremental rebakes of a dirty
// region, area volumes, and obstacle placement that reports the tiles it touched. Issue #28, tasks
// 1.1, 1.3 and 1.4 of `implement-issue-28-navigation-authoring`.
//
// `build_tile` (build.h) builds ONE tile for bounds its caller chooses. A bake is the loop over it:
// the tile layout comes from `NavBakeSettings::tile_size`, the XZ extent of tile (x, z) is
// `[x * t, (x + 1) * t]`, and the Y range comes from the source geometry, so the same inputs always
// produce the same tile bounds and therefore the same tiles. `bake_tile_bounds` exposes that rule,
// so a test (or the editor service) can build any tile directly and compare it with the baked one
// through `tile_digest` (tile_identity.h).
//
// The editor never runs any of this. The engine's editor service calls it with sources gathered by
// the runtime host and reports the result, and the document only records what came back.

#include <cy/core/base/expected.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/array.h>
#include <cy/navigation/build.h>
#include <cy/navigation/navmesh.h>

namespace cy::navigation {

/// The settings an author edits, mapped onto `NavBuildParams`, the `NavMesh` tile size and an
/// `AgentProfile`. The region, contour and detail parameters keep `NavBuildParams`' defaults.
struct NavBakeSettings {
    // The agent profile.
    f32 agent_radius = 0.5F;
    f32 agent_height = 2.0F;
    f32 max_slope_degrees = 45.0F;
    /// The step height an agent can climb: `NavBuildParams::agent_max_climb`.
    f32 step_height = 0.4F;
    // The build.
    f32 cell_size = 0.3F;
    f32 cell_height = 0.2F;
    /// The `NavMesh` constructor argument, in metres.
    f32 tile_size = 16.0F;
    u64 layers = ~u64{0};
    u64 tags = ~u64{0};
    /// Engine or Recast. `Automatic` is refused by `validate_bake_settings`, because a saved bake
    /// must name the back end that produced it.
    NavBuildBackend backend = NavBuildBackend::Engine;
};

/// Refuses an `Automatic` back end, non-positive sizes, a tile smaller than one cell, and a tile
/// with more voxel columns than `build_tile` accepts.
[[nodiscard]] Status validate_bake_settings(const NavBakeSettings& settings) noexcept;

/// The `build_tile` parameters for these settings. The include and exclude volumes are left empty;
/// the bake fills the exclusions from the source's excluding surfaces.
[[nodiscard]] NavBuildParams build_params(const NavBakeSettings& settings) noexcept;

/// The runtime agent profile the baked mesh is for, for `NavMesh::set_profile`.
[[nodiscard]] AgentProfile agent_profile(const NavBakeSettings& settings, Name name) noexcept;

/// A `NavMeshSurface`: an including surface selects the tiles a full bake covers, and an excluding
/// one removes the triangles inside it.
struct NavSurfaceVolume {
    /// The authoring node, so the order of volumes is stable whatever order the host gathers them.
    u64 node = 0;
    Aabb bounds;
    bool exclude = false;
};

/// A `NavArea`: triangles whose centroid lies inside `bounds` take `area`, and `cost` becomes that
/// area's multiplier in the world's `NavAreaCosts`. When volumes overlap, the one with the highest
/// `node` wins (the latest-authored, in a stable order).
struct NavAreaVolume {
    u64 node = 0;
    Aabb bounds;
    AreaType area = kAreaGround;
    f32 cost = 1.0F;
};

/// Everything a bake reads, as the runtime host gathers it for one navigation world.
///
/// `obstacles` and `links` are runtime overlays. The bake does not voxelise them and the source
/// fingerprint excludes them: they are applied to the baked mesh with `place_obstacle` and
/// `NavMesh::add_link`, and moving one never makes a bake stale.
struct NavBakeSource {
    NavSourceGeometry geometry;
    Span<const NavSurfaceVolume> surfaces;
    Span<const NavAreaVolume> areas;
    Span<const NavObstacleShape> obstacles;
    Span<const NavLink> links;
};

/// One tile of a bake, in the order the bake visited it: by layer, then z, then x.
struct NavBakeTile {
    TileCoord coord;
    u32 polys = 0;
    /// No walkable cell. The tile is not published and any previous occupant is removed.
    bool empty = false;
    /// `tile_digest` of the built tile.
    u64 digest = 0;
};

/// What a bake did: the `NavBuildReport` counters summed over every tile, and each tile visited.
struct NavBakeReport {
    /// The triangle counters describe the source (every tile filters all of it); spans, polygons,
    /// vertices and `duration_ns` are summed over the tiles. `backend` is the back end that ran.
    /// `duration_ns` is wall time and is diagnostic only.
    NavBuildReport totals;
    u32 tiles_built = 0;
    u32 tiles_empty = 0;
    /// True when the observer stopped the bake before every tile was visited. The tiles visited
    /// before it stopped are published and listed.
    bool cancelled = false;
    Array<NavBakeTile> tiles;

    explicit NavBakeReport(Allocator& allocator) noexcept : tiles(allocator) {}
};

/// What the observer answers after each tile.
enum class NavBakeProgress : u8 { Continue, Cancel };

/// Per-tile progress, and cooperative cancellation: the bake checks the answer after every tile.
class NavBakeObserver {
public:
    virtual ~NavBakeObserver() = default;
    /// `done` of `total` tiles are finished; `tile` is the one just finished.
    [[nodiscard]] virtual NavBakeProgress tile_baked(u32 done, u32 total,
                                                     const NavBakeTile& tile) noexcept = 0;
};

/// The coordinates of every tile whose XZ extent `[x * t, (x + 1) * t]` overlaps `region`, by z and
/// then x, at layer zero. A tile that only touches the region's edge does not overlap it. Refuses a
/// region of more than 65536 tiles.
[[nodiscard]] Status tiles_overlapping(f32 tile_size, const Aabb& region,
                                       Array<TileCoord>& out) noexcept;

/// The union of the including surfaces' bounds: the region a full bake covers. Empty when there
/// is no including surface.
[[nodiscard]] Aabb surface_region(Span<const NavSurfaceVolume> surfaces) noexcept;

/// The bounds a bake passes to `build_tile` for `coord`: the tile's XZ extent and the geometry's Y
/// range, widened by the step height and a cell below and by the agent height above.
[[nodiscard]] Aabb bake_tile_bounds(const NavBakeSettings& settings,
                                    const NavSourceGeometry& geometry, TileCoord coord) noexcept;

/// One area per source triangle: the geometry's own (or `kAreaGround`), replaced by the winning
/// area volume that contains the triangle's centroid.
[[nodiscard]] Status assign_triangle_areas(const NavBakeSource& source,
                                           Array<AreaType>& out) noexcept;

/// The world's area-cost table: uniform, with each area volume's cost on its area. When two
/// volumes name the same area, the one with the highest `node` sets the cost.
[[nodiscard]] NavAreaCosts area_costs(Span<const NavAreaVolume> areas) noexcept;

/// A full bake of `region`. Builds every overlapping tile with `build_tile` and publishes it with
/// `NavMesh::add_tile`, and removes every resident tile outside the region, so the mesh ends up
/// holding exactly the region's non-empty tiles. `mesh.tile_size()` must equal the settings' tile
/// size. `observer` may be null.
[[nodiscard]] Expected<NavBakeReport, Error> bake_tiles(Allocator& allocator,
                                                        const NavBakeSettings& settings,
                                                        const NavBakeSource& source,
                                                        const Aabb& region, NavMesh& mesh,
                                                        NavBakeObserver* observer) noexcept;

/// An incremental rebake: rebuilds only the tiles overlapping `dirty` and leaves every other tile,
/// its digest and its slot salt untouched. The report lists the rebuilt coordinates.
[[nodiscard]] Expected<NavBakeReport, Error> rebake_tiles(Allocator& allocator,
                                                          const NavBakeSettings& settings,
                                                          const NavBakeSource& source,
                                                          const Aabb& dirty, NavMesh& mesh,
                                                          NavBakeObserver* observer) noexcept;

/// The incremental counterpart of `bake_tiles` over `surface_region(source.surfaces)`, which is
/// what an editor bake runs. Of the tiles overlapping `dirty`, it rebuilds those the surface region
/// covers and removes the resident ones it no longer covers (a shrunk or moved surface); every
/// other tile keeps its digest and slot salt. A removed tile is listed as empty, with a zero
/// digest.
///
/// The result equals a full bake of the same sources when the mesh equalled a full bake of the
/// previous sources, every change between the two lies inside `dirty`, and the geometry's Y range
/// (which every tile's build box spans, see `bake_tile_bounds`) did not change. A caller that
/// cannot vouch for all three runs `bake_tiles` over the surface region instead.
[[nodiscard]] Expected<NavBakeReport, Error> rebake_surface_tiles(Allocator& allocator,
                                                                  const NavBakeSettings& settings,
                                                                  const NavBakeSource& source,
                                                                  const Aabb& dirty,
                                                                  NavMesh& mesh) noexcept;

/// An obstacle edit and the resident tiles it touched: those overlapping the old footprint, the
/// new one, or both. Nothing is voxelised, so every tile keeps its digest and salt.
struct NavObstacleChange {
    ObstacleId id = kInvalidObstacle;
    Array<TileCoord> affected;

    explicit NavObstacleChange(Allocator& allocator) noexcept : affected(allocator) {}
};

/// `NavMesh::add_obstacle`, reporting the tiles under the footprint.
[[nodiscard]] Expected<NavObstacleChange, Error> place_obstacle(
    NavMesh& mesh, const NavObstacleShape& shape) noexcept;

/// Removes obstacle `id` and adds `next` in its place, reporting the tiles under either footprint.
/// The returned id may differ from `id`.
[[nodiscard]] Expected<NavObstacleChange, Error> move_obstacle(
    NavMesh& mesh, ObstacleId id, const NavObstacleShape& next) noexcept;

/// `NavMesh::remove_obstacle`, reporting the tiles under the old footprint.
[[nodiscard]] Expected<NavObstacleChange, Error> clear_obstacle(NavMesh& mesh,
                                                                ObstacleId id) noexcept;

}  // namespace cy::navigation
