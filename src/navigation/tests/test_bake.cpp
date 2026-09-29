// SPDX-License-Identifier: MIT
// The bake API above `build_tile`: the tiling loop, incremental rebakes, area volumes, obstacle
// reports and the source fingerprint. Issue #28, tasks 1.1 to 1.5.
//
// INTEGRATION: most cases voxelise several tiles. The central case compares a bake with direct
// `build_tile` calls tile by tile over both back ends, which is issue #28's first acceptance
// criterion at the engine level.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/bake.h>
#include <cy/navigation/query.h>
#include <cy/navigation/tile_identity.h>
#include <cy/test/test.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "nav_fixture.h"

using namespace cy;
using namespace cy::navigation;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

constexpr f32 kTile = 8.0F;
constexpr AreaType kMud = 5;

[[nodiscard]] NavBakeSettings settings(NavBuildBackend backend, f32 cell = 0.25F) noexcept {
    NavBakeSettings out;
    out.backend = backend;
    out.cell_size = cell;
    out.cell_height = 0.2F;
    out.agent_radius = 0.25F;
    out.agent_height = 2.0F;
    out.step_height = 0.4F;
    out.max_slope_degrees = 45.0F;
    out.tile_size = kTile;
    return out;
}

[[nodiscard]] NavMesh mesh_for(const NavBakeSettings& bake) noexcept {
    return {allocator(), Name::intern("test.bake"), bake.tile_size};
}

[[nodiscard]] Aabb box(f32 x0, f32 z0, f32 x1, f32 z1) noexcept {
    return Aabb::from_min_max(Vec3{x0, -1.0F, z0}, Vec3{x1, 3.0F, z1});
}

/// A 16 m square of ground: four 8 m tiles.
[[nodiscard]] testing::SourceGeometry square() noexcept {
    testing::SourceGeometry source(allocator());
    source.ground(0.0F, 0.0F, 16, 16, 1.0F);
    return source;
}

[[nodiscard]] NavBakeSource source_of(const testing::SourceGeometry& geometry) noexcept {
    NavBakeSource source;
    source.geometry = geometry.geometry();
    return source;
}

/// The digest and slot salt of one resident tile.
struct TileState {
    u64 digest = 0;
    u32 salt = 0;
};

[[nodiscard]] TileState state_of(const NavMesh& mesh, TileCoord coord) noexcept {
    const u32 slot = mesh.tile_slot(coord);
    if (slot == 0xFFFFFFFFU) {
        return {};
    }
    return TileState{mesh_tile_digest(mesh, slot), mesh.tile_poly(slot, 0).salt()};
}

[[nodiscard]] bool has(Span<const TileCoord> coords, TileCoord coord) noexcept {
    return std::ranges::any_of(coords,
                               [coord](const TileCoord& candidate) { return candidate == coord; });
}

struct Counter final : NavBakeObserver {
    u32 calls = 0;
    u32 last_done = 0;
    u32 last_total = 0;
    u32 cancel_after = 0xFFFFFFFFU;

    NavBakeProgress tile_baked(u32 done, u32 total, const NavBakeTile&) noexcept override {
        ++calls;
        last_done = done;
        last_total = total;
        return (calls >= cancel_after) ? NavBakeProgress::Cancel : NavBakeProgress::Continue;
    }
};

/// Compare every tile of a bake with a direct `build_tile` of the same inputs and bounds.
void check_bake_equals_build_tile(NavBuildBackend backend) noexcept {
    const NavBakeSettings bake = settings(backend);
    const testing::SourceGeometry geometry = square();
    const NavBakeSource source = source_of(geometry);
    NavMesh mesh = mesh_for(bake);
    Counter counter;

    Expected<NavBakeReport, Error> report =
        bake_tiles(allocator(), bake, source, box(0.0F, 0.0F, 16.0F, 16.0F), mesh, &counter);
    CY_REQUIRE(report.has_value());
    if (!report.has_value()) {
        return;
    }
    CY_CHECK_EQ(report->totals.backend, backend);
    CY_CHECK_EQ(report->tiles_built, 4U);
    CY_CHECK_EQ(report->tiles_empty, 0U);
    CY_CHECK_EQ(mesh.tile_count(), 4U);
    CY_CHECK_EQ(counter.calls, 4U);
    CY_CHECK_EQ(counter.last_total, 4U);

    Array<AreaType> areas(allocator());
    CY_REQUIRE(assign_triangle_areas(source, areas).has_value());
    NavSourceGeometry direct_geometry = source.geometry;
    direct_geometry.area = areas.span();

    Array<TileCoord> coords(allocator());
    CY_REQUIRE(tiles_overlapping(kTile, box(0.0F, 0.0F, 16.0F, 16.0F), coords).has_value());
    CY_REQUIRE_EQ(coords.size(), report->tiles.size());
    for (usize index = 0; index < coords.size() && index < report->tiles.size(); ++index) {
        const TileCoord coord = coords[index];
        NavBuildReport direct_report;
        Expected<NavTileData, Error> direct =
            build_tile(allocator(), build_params(bake), direct_geometry, coord,
                       bake_tile_bounds(bake, source.geometry, coord), direct_report);
        CY_REQUIRE(direct.has_value());
        if (!direct.has_value()) {
            continue;
        }
        const u64 expected = tile_digest(*direct);
        CY_CHECK(report->tiles[index].coord == coord);
        CY_CHECK_EQ(report->tiles[index].digest, expected);
        CY_CHECK_GT(report->tiles[index].polys, 0U);
        const u32 slot = mesh.tile_slot(coord);
        CY_REQUIRE_NE(slot, 0xFFFFFFFFU);
        CY_CHECK_EQ(mesh_tile_digest(mesh, slot), expected);
    }
}

}  // namespace

CY_TEST_CASE("a bake equals build_tile tile by tile on the engine back end") {
    check_bake_equals_build_tile(NavBuildBackend::Engine);
}

CY_TEST_CASE("a bake equals build_tile tile by tile on the Recast back end") {
    if (!recast_available()) {
        CY_TEST_MESSAGE("SKIP: this build has no Recast (CY_NAVIGATION is off)");
        return;
    }
    check_bake_equals_build_tile(NavBuildBackend::Recast);
}

CY_TEST_CASE("bake settings refuse an automatic back end and a mismatched mesh") {
    NavBakeSettings bake = settings(NavBuildBackend::Engine);
    bake.backend = NavBuildBackend::Automatic;
    CY_CHECK_FALSE(validate_bake_settings(bake).has_value());
    bake.backend = NavBuildBackend::Engine;
    bake.cell_size = 0.0F;
    CY_CHECK_FALSE(validate_bake_settings(bake).has_value());

    const testing::SourceGeometry geometry = square();
    NavMesh wrong(allocator(), Name::intern("test.bake"), kTile * 2.0F);
    const Expected<NavBakeReport, Error> refused =
        bake_tiles(allocator(), settings(NavBuildBackend::Engine), source_of(geometry),
                   box(0.0F, 0.0F, 16.0F, 16.0F), wrong, nullptr);
    CY_CHECK_FALSE(refused.has_value());

    const AgentProfile profile =
        agent_profile(settings(NavBuildBackend::Engine), Name::intern("walker"));
    CY_CHECK_EQ(profile.radius, 0.25F);
    CY_CHECK_EQ(profile.max_climb, 0.4F);
    const NavBuildParams params = build_params(settings(NavBuildBackend::Engine));
    CY_CHECK_EQ(params.agent_max_climb, 0.4F);
    CY_CHECK_EQ(params.backend, NavBuildBackend::Engine);
}

CY_TEST_CASE("an empty tile inside the region is reported and the bake succeeds") {
    const NavBakeSettings bake = settings(NavBuildBackend::Engine);
    const testing::SourceGeometry geometry = square();
    NavMesh mesh = mesh_for(bake);

    // Three tiles across, but the ground stops at x = 16: the column x = 2 has nothing to walk on.
    Expected<NavBakeReport, Error> report = bake_tiles(
        allocator(), bake, source_of(geometry), box(0.0F, 0.0F, 24.0F, 16.0F), mesh, nullptr);
    CY_REQUIRE(report.has_value());
    if (!report.has_value()) {
        return;
    }
    CY_CHECK_EQ(report->tiles.size(), usize{6});
    CY_CHECK_EQ(report->tiles_built, 4U);
    CY_CHECK_EQ(report->tiles_empty, 2U);
    for (const NavBakeTile& tile : report->tiles.span()) {
        CY_CHECK_EQ(tile.empty, tile.coord.x == 2);
    }
    CY_CHECK_EQ(mesh.tile_count(), 4U);
    CY_CHECK_FALSE(mesh.tile_resident(TileCoord{2, 0, 0}));

    // A full bake of a smaller region leaves only that region's tiles.
    Expected<NavBakeReport, Error> smaller = bake_tiles(allocator(), bake, source_of(geometry),
                                                        box(0.0F, 0.0F, 8.0F, 8.0F), mesh, nullptr);
    CY_REQUIRE(smaller.has_value());
    CY_CHECK_EQ(mesh.tile_count(), 1U);
    CY_CHECK(mesh.tile_resident(TileCoord{0, 0, 0}));
}

CY_TEST_CASE("an observer cancels a bake after the tile it is told about") {
    const NavBakeSettings bake = settings(NavBuildBackend::Engine);
    const testing::SourceGeometry geometry = square();
    NavMesh mesh = mesh_for(bake);
    Counter counter;
    counter.cancel_after = 1;

    Expected<NavBakeReport, Error> report = bake_tiles(
        allocator(), bake, source_of(geometry), box(0.0F, 0.0F, 16.0F, 16.0F), mesh, &counter);
    CY_REQUIRE(report.has_value());
    if (!report.has_value()) {
        return;
    }
    CY_CHECK(report->cancelled);
    CY_CHECK_EQ(counter.calls, 1U);
    CY_CHECK_EQ(counter.last_done, 1U);
    CY_CHECK_EQ(counter.last_total, 4U);
    CY_CHECK_EQ(report->tiles.size(), usize{1});
    CY_CHECK_EQ(mesh.tile_count(), 1U);
}

CY_TEST_CASE("tiles overlapping a region exclude the tiles that only touch its edge") {
    Array<TileCoord> coords(allocator());
    CY_REQUIRE(tiles_overlapping(kTile, box(0.0F, 0.0F, 8.0F, 8.0F), coords).has_value());
    CY_CHECK_EQ(coords.size(), usize{1});
    CY_REQUIRE(tiles_overlapping(kTile, box(-0.5F, 7.5F, 8.5F, 8.5F), coords).has_value());
    CY_CHECK_EQ(coords.size(), usize{6});
    CY_CHECK(has(coords.span(), TileCoord{-1, 0, 0}));
    CY_CHECK(has(coords.span(), TileCoord{1, 1, 0}));
    CY_REQUIRE(tiles_overlapping(kTile, Aabb::empty(), coords).has_value());
    CY_CHECK(coords.empty());
    CY_CHECK_FALSE(tiles_overlapping(kTile, box(0.0F, 0.0F, 1.0e6F, 1.0e6F), coords).has_value());
}

CY_TEST_CASE("an area volume edit rebakes only its tile and the others keep digest and salt") {
    const NavBakeSettings bake = settings(NavBuildBackend::Engine);
    const testing::SourceGeometry geometry = square();
    NavMesh mesh = mesh_for(bake);
    CY_REQUIRE(bake_tiles(allocator(), bake, source_of(geometry), box(0.0F, 0.0F, 16.0F, 16.0F),
                          mesh, nullptr)
                   .has_value());

    const TileCoord coords[4] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    TileState before[4];
    for (usize index = 0; index < 4; ++index) {
        before[index] = state_of(mesh, coords[index]);
    }

    const NavAreaVolume mud{1, box(2.0F, 2.0F, 5.0F, 5.0F), kMud, 4.0F};
    NavBakeSource edited = source_of(geometry);
    edited.areas = Span<const NavAreaVolume>(&mud, 1);
    Expected<NavBakeReport, Error> report =
        rebake_tiles(allocator(), bake, edited, mud.bounds, mesh, nullptr);
    CY_REQUIRE(report.has_value());
    if (!report.has_value()) {
        return;
    }
    CY_REQUIRE_EQ(report->tiles.size(), usize{1});
    if (report->tiles.size() != 1) {
        return;
    }
    CY_CHECK(report->tiles[0].coord == coords[0]);

    const TileState rebuilt = state_of(mesh, coords[0]);
    CY_CHECK_NE(rebuilt.digest, before[0].digest);
    CY_CHECK_NE(rebuilt.salt, before[0].salt);
    for (usize index = 1; index < 4; ++index) {
        const TileState now = state_of(mesh, coords[index]);
        CY_CHECK_EQ(now.digest, before[index].digest);
        CY_CHECK_EQ(now.salt, before[index].salt);
    }

    // The rebuilt tile carries the volume's area on the polygons inside it, and only there.
    const u32 slot = mesh.tile_slot(coords[0]);
    u32 muddy = 0;
    for (u32 index = 0; index < mesh.tile_poly_count(slot); ++index) {
        const NavPoly* poly = mesh.poly(mesh.tile_poly(slot, index));
        const bool inside = box(1.75F, 1.75F, 5.25F, 5.25F).contains(poly->centre);
        muddy += (poly->area == kMud) ? 1U : 0U;
        CY_CHECK(((poly->area != kMud) || inside));
    }
    CY_CHECK_GT(muddy, 0U);
}

CY_TEST_CASE("area volumes assign their area to triangles by centroid, highest node winning") {
    testing::SourceGeometry geometry(allocator());
    // Three triangles with centroids at x = 1, 3 and 5, and a fourth at x = 9 with its own area.
    for (u32 index = 0; index < 3; ++index) {
        const f32 x = static_cast<f32>(index) * 2.0F;
        geometry.triangle(Vec3{x, 0.0F, 0.0F}, Vec3{x + 1.5F, 0.0F, 1.5F},
                          Vec3{x + 1.5F, 0.0F, -1.5F});
    }
    geometry.triangle(Vec3{8.0F, 0.0F, 0.0F}, Vec3{9.5F, 0.0F, 1.5F}, Vec3{9.5F, 0.0F, -1.5F}, 9);

    const NavAreaVolume volumes[2] = {{7, box(0.0F, -1.0F, 3.5F, 1.0F), 3, 2.0F},
                                      {2, box(2.5F, -1.0F, 6.0F, 1.0F), 4, 5.0F}};
    NavBakeSource source = source_of(geometry);
    source.areas = Span<const NavAreaVolume>(volumes, 2);
    Array<AreaType> areas(allocator());
    CY_REQUIRE(assign_triangle_areas(source, areas).has_value());
    CY_REQUIRE_EQ(areas.size(), usize{4});
    if (areas.size() != 4) {
        return;
    }
    CY_CHECK_EQ(areas[0], AreaType{3});
    CY_CHECK_EQ(areas[1], AreaType{3});  // both contain it; node 7 beats node 2
    CY_CHECK_EQ(areas[2], AreaType{4});
    CY_CHECK_EQ(areas[3], AreaType{9});  // outside every volume: its own area

    // The winner does not depend on the order the host gathered the volumes in.
    const NavAreaVolume reversed[2] = {volumes[1], volumes[0]};
    source.areas = Span<const NavAreaVolume>(reversed, 2);
    CY_REQUIRE(assign_triangle_areas(source, areas).has_value());
    CY_CHECK_EQ(areas[1], AreaType{3});

    const NavAreaCosts costs = area_costs(Span<const NavAreaVolume>(volumes, 2));
    CY_CHECK_EQ(costs.of(3), 2.0F);
    CY_CHECK_EQ(costs.of(4), 5.0F);
    CY_CHECK_EQ(costs.of(kAreaGround), 1.0F);
}

CY_TEST_CASE("an area volume's cost changes the cost of a path across it") {
    NavBakeSettings bake = settings(NavBuildBackend::Engine, 0.5F);
    bake.tile_size = 16.0F;
    testing::SourceGeometry geometry(allocator());
    geometry.ground(0.0F, 0.0F, 16, 16, 1.0F);
    // A strip of mud across the whole square, so every path from west to east crosses it.
    const NavAreaVolume mud{1, box(6.0F, -1.0F, 10.0F, 17.0F), kMud, 10.0F};
    NavBakeSource source = source_of(geometry);
    source.areas = Span<const NavAreaVolume>(&mud, 1);
    NavMesh mesh(allocator(), Name::intern("test.bake"), bake.tile_size);
    CY_REQUIRE(bake_tiles(allocator(), bake, source, box(0.0F, 0.0F, 16.0F, 16.0F), mesh, nullptr)
                   .has_value());

    const Vec3 start{2.0F, 0.0F, 8.0F};
    const Vec3 end{14.0F, 0.0F, 8.0F};
    const Vec3 extents{0.5F, 1.0F, 0.5F};
    PathFilter uniform;
    uniform.node_budget = 8192;
    PathFilter weighted = uniform;
    weighted.costs = area_costs(source.areas);

    PathCorridor plain(allocator());
    PathCorridor muddy(allocator());
    const PathResult cheap = find_path(mesh, start, end, extents, uniform, plain);
    const PathResult dear = find_path(mesh, start, end, extents, weighted, muddy);
    CY_REQUIRE(cheap.found);
    CY_REQUIRE(dear.found);
    CY_CHECK_FALSE(cheap.partial);
    CY_CHECK_FALSE(dear.partial);
    // Four metres of the twelve are mud at ten times the cost.
    CY_CHECK_GT(dear.cost, cheap.cost * 3.0F);
}

CY_TEST_CASE("an obstacle over a corridor blocks the path and removing it restores the cost") {
    const NavBakeSettings bake = settings(NavBuildBackend::Engine);
    testing::SourceGeometry geometry(allocator());
    geometry.ground(0.0F, 0.0F, 16, 4, 1.0F);  // a 4 m corridor along x, over tiles (0,0), (1,0)
    NavMesh mesh = mesh_for(bake);
    CY_REQUIRE(bake_tiles(allocator(), bake, source_of(geometry), box(0.0F, 0.0F, 16.0F, 4.0F),
                          mesh, nullptr)
                   .has_value());
    CY_REQUIRE_EQ(mesh.tile_count(), 2U);
    const TileState west = state_of(mesh, TileCoord{0, 0, 0});
    const TileState east = state_of(mesh, TileCoord{1, 0, 0});

    const Vec3 start{2.0F, 0.0F, 2.0F};
    const Vec3 end{14.0F, 0.0F, 2.0F};
    const Vec3 extents{0.5F, 1.0F, 0.5F};
    PathFilter filter;
    filter.node_budget = 8192;
    PathCorridor corridor(allocator());
    const PathResult open = find_path(mesh, start, end, extents, filter, corridor);
    CY_REQUIRE(open.found);
    CY_CHECK_FALSE(open.partial);

    NavObstacleShape wall;
    wall.bounds = box(11.0F, -1.0F, 13.0F, 5.0F);
    Expected<NavObstacleChange, Error> placed = place_obstacle(mesh, wall);
    CY_REQUIRE(placed.has_value());
    if (!placed.has_value()) {
        return;
    }
    CY_CHECK_EQ(placed->affected.size(), usize{1});
    CY_CHECK(has(placed->affected.span(), TileCoord{1, 0, 0}));
    const PathResult blocked = find_path(mesh, start, end, extents, filter, corridor);
    CY_CHECK((!blocked.found || blocked.partial));

    // No voxel rebuild: both tiles keep their digest and salt.
    CY_CHECK_EQ(state_of(mesh, TileCoord{0, 0, 0}).digest, west.digest);
    CY_CHECK_EQ(state_of(mesh, TileCoord{1, 0, 0}).salt, east.salt);

    Expected<NavObstacleChange, Error> cleared = clear_obstacle(mesh, placed->id);
    CY_REQUIRE(cleared.has_value());
    const PathResult restored = find_path(mesh, start, end, extents, filter, corridor);
    CY_CHECK(restored.found);
    CY_CHECK_FALSE(restored.partial);
    CY_CHECK_EQ(restored.cost, open.cost);
    CY_CHECK_EQ(state_of(mesh, TileCoord{1, 0, 0}).digest, east.digest);
}

CY_TEST_CASE("moving an obstacle reports the tiles under its old and new footprints") {
    const NavBakeSettings bake = settings(NavBuildBackend::Engine);
    testing::SourceGeometry geometry(allocator());
    geometry.ground(0.0F, 0.0F, 24, 4, 1.0F);  // three tiles along x
    NavMesh mesh = mesh_for(bake);
    CY_REQUIRE(bake_tiles(allocator(), bake, source_of(geometry), box(0.0F, 0.0F, 24.0F, 4.0F),
                          mesh, nullptr)
                   .has_value());
    CY_REQUIRE_EQ(mesh.tile_count(), 3U);

    NavObstacleShape shape;
    shape.bounds = box(2.0F, 1.0F, 3.0F, 2.0F);
    Expected<NavObstacleChange, Error> placed = place_obstacle(mesh, shape);
    CY_REQUIRE(placed.has_value());
    if (!placed.has_value()) {
        return;
    }
    CY_CHECK_EQ(placed->affected.size(), usize{1});

    // From tile 0 to tile 2: both, and not the tile in between.
    NavObstacleShape far = shape;
    far.bounds = box(18.0F, 1.0F, 19.0F, 2.0F);
    Expected<NavObstacleChange, Error> moved = move_obstacle(mesh, placed->id, far);
    CY_REQUIRE(moved.has_value());
    if (!moved.has_value()) {
        return;
    }
    CY_CHECK_EQ(moved->affected.size(), usize{2});
    CY_CHECK(has(moved->affected.span(), TileCoord{0, 0, 0}));
    CY_CHECK(has(moved->affected.span(), TileCoord{2, 0, 0}));
    CY_CHECK_FALSE(has(moved->affected.span(), TileCoord{1, 0, 0}));

    // Across the seam between tiles 1 and 2: the new footprint's two tiles.
    NavObstacleShape seam = shape;
    seam.bounds = box(15.0F, 1.0F, 17.0F, 2.0F);
    Expected<NavObstacleChange, Error> across = move_obstacle(mesh, moved->id, seam);
    CY_REQUIRE(across.has_value());
    if (!across.has_value()) {
        return;
    }
    CY_CHECK_EQ(across->affected.size(), usize{2});
    CY_CHECK(has(across->affected.span(), TileCoord{1, 0, 0}));
    CY_CHECK(has(across->affected.span(), TileCoord{2, 0, 0}));
    CY_CHECK_EQ(mesh.obstacle_count(), 1U);
    CY_CHECK_FALSE(move_obstacle(mesh, 99, seam).has_value());
}

CY_TEST_CASE("the source fingerprint follows the bake inputs and ignores obstacles and links") {
    const NavBakeSettings bake = settings(NavBuildBackend::Engine);
    testing::SourceGeometry geometry = square();
    const NavAreaVolume mud{1, box(2.0F, 2.0F, 5.0F, 5.0F), kMud, 4.0F};
    const NavSurfaceVolume surface{3, box(0.0F, 0.0F, 16.0F, 16.0F), false};
    NavBakeSource source = source_of(geometry);
    source.areas = Span<const NavAreaVolume>(&mud, 1);
    source.surfaces = Span<const NavSurfaceVolume>(&surface, 1);
    const u64 base = source_fingerprint(bake, source, 1);
    CY_CHECK_EQ(source_fingerprint(bake, source, 1), base);

    // Runtime overlays: not part of the fingerprint.
    NavObstacleShape obstacle;
    obstacle.bounds = box(1.0F, 1.0F, 2.0F, 2.0F);
    NavLink link;
    link.from = Vec3{1.0F, 0.0F, 1.0F};
    link.to = Vec3{9.0F, 0.0F, 9.0F};
    NavBakeSource overlaid = source;
    overlaid.obstacles = Span<const NavObstacleShape>(&obstacle, 1);
    overlaid.links = Span<const NavLink>(&link, 1);
    CY_CHECK_EQ(source_fingerprint(bake, overlaid, 1), base);

    // A vertex, a volume, a setting, the back end and the producer version each change it.
    geometry.vertices[5].y += 0.25F;
    CY_CHECK_NE(source_fingerprint(bake, source_of(geometry), 1),
                source_fingerprint(bake, source_of(square()), 1));
    geometry.vertices[5].y -= 0.25F;

    NavAreaVolume moved_mud = mud;
    moved_mud.bounds.max.x += 1.0F;
    NavBakeSource moved = source;
    moved.areas = Span<const NavAreaVolume>(&moved_mud, 1);
    CY_CHECK_NE(source_fingerprint(bake, moved, 1), base);

    NavSurfaceVolume excluded = surface;
    excluded.exclude = true;
    NavBakeSource carved = source;
    carved.surfaces = Span<const NavSurfaceVolume>(&excluded, 1);
    CY_CHECK_NE(source_fingerprint(bake, carved, 1), base);

    NavBakeSettings finer = bake;
    finer.cell_size = 0.2F;
    CY_CHECK_NE(source_fingerprint(finer, source, 1), base);
    NavBakeSettings recast = bake;
    recast.backend = NavBuildBackend::Recast;
    CY_CHECK_NE(source_fingerprint(recast, source, 1), base);
    CY_CHECK_NE(source_fingerprint(bake, source, 2), base);
}

CY_TEST_CASE("the bake identity names the fingerprint and the ordered tile digests") {
    const NavBakeSettings bake = settings(NavBuildBackend::Engine);
    const testing::SourceGeometry geometry = square();
    NavMesh mesh = mesh_for(bake);
    Expected<NavBakeReport, Error> report = bake_tiles(
        allocator(), bake, source_of(geometry), box(0.0F, 0.0F, 16.0F, 16.0F), mesh, nullptr);
    CY_REQUIRE(report.has_value());
    if (!report.has_value()) {
        return;
    }
    Array<u64> digests(allocator());
    for (const NavBakeTile& tile : report->tiles.span()) {
        CY_REQUIRE(digests.push_back(tile.digest).has_value());
    }
    const u64 fingerprint = source_fingerprint(bake, source_of(geometry), 1);
    const Expected<u64, Error> from_mesh = mesh_bake_identity(fingerprint, mesh);
    CY_REQUIRE(from_mesh.has_value());
    if (!from_mesh.has_value()) {
        return;
    }
    CY_CHECK_EQ(*from_mesh, bake_identity(fingerprint, digests.span()));
    CY_CHECK_NE(bake_identity(fingerprint + 1, digests.span()), *from_mesh);
    CY_REQUIRE_EQ(digests.size(), usize{4});
    if (digests.size() != 4) {
        return;
    }
    const u64 swapped[4] = {digests[1], digests[0], digests[2], digests[3]};
    CY_CHECK_NE(bake_identity(fingerprint, Span<const u64>(swapped, 4)), *from_mesh);
}

CY_TEST_CASE("a two-tile Recast bake connects its tiles across the seam") {
    if (!recast_available()) {
        CY_TEST_MESSAGE("SKIP: this build has no Recast (CY_NAVIGATION is off)");
        return;
    }
    const NavBakeSettings bake = settings(NavBuildBackend::Recast);
    testing::SourceGeometry geometry(allocator());
    geometry.ground(0.0F, 0.0F, 16, 4, 1.0F);  // a 4 m corridor along x, over tiles (0,0), (1,0)
    NavMesh mesh = mesh_for(bake);
    CY_REQUIRE(bake_tiles(allocator(), bake, source_of(geometry), box(0.0F, 0.0F, 16.0F, 4.0F),
                          mesh, nullptr)
                   .has_value());
    CY_REQUIRE_EQ(mesh.tile_count(), 2U);

    // The border is voxelised and discarded: every polygon stays inside its own tile, and the two
    // tiles meet on the seam at x = 8.
    for (u32 slot = 0; slot < mesh.tile_capacity(); ++slot) {
        const TileCoord coord = mesh.tile_coord(slot);
        if (mesh.tile_slot(coord) != slot) {
            continue;
        }
        const f32 low = static_cast<f32>(coord.x) * kTile;
        for (u32 index = 0; index < mesh.tile_poly_count(slot); ++index) {
            Vec3 corners[kMaxPolyVertices] = {};
            const u32 count =
                mesh.poly_vertices(mesh.tile_poly(slot, index), corners, kMaxPolyVertices);
            for (u32 corner = 0; corner < count; ++corner) {
                CY_CHECK_GE(corners[corner].x, low - 0.01F);
                CY_CHECK_LE(corners[corner].x, low + kTile + 0.01F);
            }
        }
    }

    PathFilter filter;
    filter.node_budget = 8192;
    PathCorridor corridor(allocator());
    const PathResult path = find_path(mesh, Vec3{2.0F, 0.0F, 2.0F}, Vec3{14.0F, 0.0F, 2.0F},
                                      Vec3{0.5F, 1.0F, 0.5F}, filter, corridor);
    CY_CHECK(path.found);
    CY_CHECK_FALSE(path.partial);
}

namespace {

/// Every resident tile's digest, by coordinate.
[[nodiscard]] std::vector<std::pair<TileCoord, u64>> resident_digests(const NavMesh& mesh) {
    std::vector<std::pair<TileCoord, u64>> out;
    for (u32 slot = 0; slot < mesh.tile_capacity(); ++slot) {
        const TileCoord coord = mesh.tile_coord(slot);
        if (mesh.tile_slot(coord) == slot) {
            out.emplace_back(coord, mesh_tile_digest(mesh, slot));
        }
    }
    std::ranges::sort(out, [](const auto& a, const auto& b) {
        return a.first.z != b.first.z ? a.first.z < b.first.z : a.first.x < b.first.x;
    });
    return out;
}

/// The mesh a full bake of `source`'s surface region produces.
[[nodiscard]] std::vector<std::pair<TileCoord, u64>> fresh_bake(const NavBakeSettings& bake,
                                                                const NavBakeSource& source) {
    NavMesh fresh = mesh_for(bake);
    CY_REQUIRE(
        bake_tiles(allocator(), bake, source, surface_region(source.surfaces), fresh, nullptr)
            .has_value());
    return resident_digests(fresh);
}

}  // namespace

CY_TEST_CASE("an incremental surface rebake after a surface shrinks equals a fresh bake") {
    const NavBakeSettings bake = settings(NavBuildBackend::Engine);
    const testing::SourceGeometry geometry = square();
    const NavSurfaceVolume whole{1, box(0.0F, 0.0F, 16.0F, 16.0F), false};
    NavBakeSource source = source_of(geometry);
    source.surfaces = Span<const NavSurfaceVolume>(&whole, 1);
    NavMesh mesh = mesh_for(bake);
    CY_REQUIRE(bake_tiles(allocator(), bake, source, surface_region(source.surfaces), mesh, nullptr)
                   .has_value());
    CY_REQUIRE_EQ(mesh.tile_count(), 4U);

    // The surface shrinks to the west half. The host dirties the union of its old and new bounds.
    const NavSurfaceVolume west{1, box(0.0F, 0.0F, 8.0F, 16.0F), false};
    source.surfaces = Span<const NavSurfaceVolume>(&west, 1);
    Expected<NavBakeReport, Error> report =
        rebake_surface_tiles(allocator(), bake, source, merge(whole.bounds, west.bounds), mesh);
    CY_REQUIRE(report.has_value());
    if (!report.has_value()) {
        return;
    }
    CY_CHECK_EQ(report->tiles_built, 2U);
    CY_CHECK_EQ(report->tiles_empty, 2U);
    CY_CHECK_FALSE(mesh.tile_resident(TileCoord{1, 0, 0}));
    CY_CHECK_FALSE(mesh.tile_resident(TileCoord{1, 1, 0}));
    CY_CHECK(resident_digests(mesh) == fresh_bake(bake, source));
}

CY_TEST_CASE("an incremental surface rebake ignores a volume that reaches past the surface") {
    const NavBakeSettings bake = settings(NavBuildBackend::Engine);
    testing::SourceGeometry geometry(allocator());
    geometry.ground(0.0F, 0.0F, 24, 8, 1.0F);  // ground over three tiles along x
    const NavSurfaceVolume surface{1, box(0.0F, 0.0F, 8.0F, 8.0F), false};
    NavBakeSource source = source_of(geometry);
    source.surfaces = Span<const NavSurfaceVolume>(&surface, 1);
    NavMesh mesh = mesh_for(bake);
    CY_REQUIRE(bake_tiles(allocator(), bake, source, surface_region(source.surfaces), mesh, nullptr)
                   .has_value());
    CY_REQUIRE_EQ(mesh.tile_count(), 1U);

    // A mud volume from x = 4 to x = 20: its dirty box covers three tiles, the surface one.
    const NavAreaVolume mud{2, box(4.0F, 0.0F, 20.0F, 8.0F), kMud, 3.0F};
    source.areas = Span<const NavAreaVolume>(&mud, 1);
    Expected<NavBakeReport, Error> report =
        rebake_surface_tiles(allocator(), bake, source, mud.bounds, mesh);
    CY_REQUIRE(report.has_value());
    if (!report.has_value()) {
        return;
    }
    CY_CHECK_EQ(report->tiles_built, 1U);
    CY_CHECK_EQ(mesh.tile_count(), 1U);
    CY_CHECK(resident_digests(mesh) == fresh_bake(bake, source));
}
