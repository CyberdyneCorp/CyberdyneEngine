// Every navigation debug family emits a renderer-independent command.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/debug.h>
#include <cy/test/test.h>

#include "nav_fixture.h"

using namespace cy;
using namespace cy::navigation;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

struct Recorder final : NavDebugSink {
    u32 polygons = 0;
    u32 tiles = 0;
    u32 adjacencies = 0;
    u32 cross_tile_adjacencies = 0;
    u32 links = 0;
    u32 corridors = 0;
    u32 paths = 0;
    u32 velocities = 0;
    u32 neighbours = 0;
    u32 obstacles = 0;
    bool saw_carved_area = false;

    void polygon(PolyRef, Span<const Vec3> corners, AreaType area) noexcept override {
        CY_CHECK_EQ(corners.size(), usize{4});
        ++polygons;
        saw_carved_area = saw_carved_area || area == kAreaNull;
    }
    void tile(TileCoord, Aabb) noexcept override { ++tiles; }
    void adjacency(PolyRef a, PolyRef b, Vec3, Vec3) noexcept override {
        ++adjacencies;
        cross_tile_adjacencies += a.tile() != b.tile() ? 1U : 0U;
    }
    void link(LinkId, Vec3, Vec3, Name) noexcept override { ++links; }
    void corridor(PolyRef) noexcept override { ++corridors; }
    void path_segment(Vec3, Vec3) noexcept override { ++paths; }
    void velocity(u64, Vec3, Vec3, Vec3) noexcept override { ++velocities; }
    void neighbour(u64, u64, Vec3, Vec3) noexcept override { ++neighbours; }
    void obstacle(ObstacleId, const NavObstacleShape&) noexcept override { ++obstacles; }
};

}  // namespace

CY_TEST_CASE("navigation debug emits polygons tiles adjacency links and obstacle footprints") {
    NavMesh mesh = testing::single_tile_mesh(allocator());
    NavObstacleShape obstacle;
    obstacle.bounds = Aabb::from_min_max(Vec3{0.0F, -1.0F, 0.0F}, Vec3{2.0F, 1.0F, 2.0F});
    CY_REQUIRE(mesh.add_obstacle(obstacle).has_value());
    NavLink link;
    link.from = Vec3{2.5F, 0.0F, 2.5F};
    link.to = Vec3{6.5F, 0.0F, 6.5F};
    link.action = Name::intern("jump");
    CY_REQUIRE(mesh.add_link(link, Vec3{0.5F, 0.5F, 0.5F}).has_value());

    Recorder recorder;
    draw_navigation_mesh(mesh, NavDebugFlags::All, recorder);
    CY_CHECK_EQ(recorder.polygons, 16U);
    CY_CHECK_EQ(recorder.tiles, 1U);
    CY_CHECK_EQ(recorder.adjacencies, 24U);
    CY_CHECK_EQ(recorder.links, 1U);
    CY_CHECK_EQ(recorder.obstacles, 1U);
    CY_CHECK(recorder.saw_carved_area);

    Recorder only_tiles;
    draw_navigation_mesh(mesh, NavDebugFlags::Tiles, only_tiles);
    CY_CHECK_EQ(only_tiles.tiles, 1U);
    CY_CHECK_EQ(only_tiles.polygons, 0U);
    CY_CHECK_EQ(only_tiles.adjacencies, 0U);
    CY_CHECK_EQ(only_tiles.links, 0U);
    CY_CHECK_EQ(only_tiles.obstacles, 0U);
}

CY_TEST_CASE("navigation debug exposes a disconnected gap behind a partial path") {
    NavMesh mesh(allocator(), Name::intern("test.debug.gap"), 8.0F);
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 8.0F, 4)).has_value());
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{2, 0, 0}, 8.0F, 4)).has_value());
    PathCorridor corridor(allocator());
    const PathResult result = find_path(mesh, Vec3{1.0F, 0.0F, 1.0F}, Vec3{17.0F, 0.0F, 1.0F},
                                        Vec3{0.2F, 0.2F, 0.2F}, PathFilter{}, corridor);
    CY_REQUIRE(result.found);
    CY_CHECK(result.partial);
    Recorder recorder;
    draw_navigation_mesh(
        mesh, NavDebugFlags::Polygons | NavDebugFlags::Tiles | NavDebugFlags::Adjacency, recorder);
    CY_CHECK_EQ(recorder.tiles, 2U);
    CY_CHECK_EQ(recorder.polygons, 32U);
    CY_CHECK_EQ(recorder.adjacencies, 48U);
    CY_CHECK_EQ(recorder.cross_tile_adjacencies, 0U);
}

CY_TEST_CASE("navigation debug emits path corridors avoidance velocities and neighbour sets") {
    NavMesh mesh = testing::single_tile_mesh(allocator());
    PathCorridor corridor(allocator());
    const Vec3 start{0.5F, 0.0F, 0.5F};
    const Vec3 end{7.5F, 0.0F, 7.5F};
    const PathResult result =
        find_path(mesh, start, end, Vec3{0.1F, 0.2F, 0.1F}, PathFilter{}, corridor);
    CY_REQUIRE(result.found);
    Array<PathPoint> points(allocator());
    CY_REQUIRE(straighten(mesh, corridor, start, end, points).has_value());
    Recorder recorder;
    draw_navigation_path(corridor, points.span(), NavDebugFlags::All, recorder);
    CY_CHECK_EQ(recorder.corridors, corridor.size());
    CY_CHECK_EQ(recorder.paths, points.size() - 1);

    const NavDebugNeighbour neighbour{2, Vec3{2.0F, 0.0F, 0.0F}};
    const NavDebugAgent agent{1, start, Vec3{1.0F, 0.0F, 0.0F}, Vec3{0.5F, 0.0F, 0.5F},
                              Span<const NavDebugNeighbour>(&neighbour, 1)};
    draw_navigation_agents(Span<const NavDebugAgent>(&agent, 1), NavDebugFlags::All, recorder);
    CY_CHECK_EQ(recorder.velocities, 1U);
    CY_CHECK_EQ(recorder.neighbours, 1U);
}

CY_TEST_CASE("path queue diagnostics count queries timing and path length") {
    NavMesh mesh = testing::single_tile_mesh(allocator());
    NavMetrics metrics;
    PathQueue queue(allocator(), mesh, 1);
    queue.set_metrics(&metrics);
    const auto id = queue.submit(1, Vec3{0.5F, 0.0F, 0.5F}, Vec3{7.5F, 0.0F, 7.5F},
                                 Vec3{0.1F, 0.2F, 0.1F}, PathFilter{}, 10);
    CY_REQUIRE(id.has_value());
    CY_CHECK_EQ(queue.update(11), 1U);
    const NavStatistics stats = metrics.snapshot();
    CY_CHECK_EQ(stats.queries, 1U);
    CY_CHECK_EQ(stats.failed_queries, 0U);
    CY_CHECK_GT(stats.query_time_ns, 0U);
    CY_CHECK_GT(stats.mean_path_length(), 9.0);
}
