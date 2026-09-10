// Path queries: A* over the polygon graph, the funnel, area preferences, budgets and partial
// results, off-mesh links, and the deterministic asynchronous queue. M8.b task 6.1.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/query.h>
#include <cy/test/test.h>

#include "nav_fixture.h"

using namespace cy;
using namespace cy::navigation;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// Two tiles side by side, so a path has somewhere to go.
[[nodiscard]] NavMesh two_tile_mesh() noexcept {
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    const Expected<TileChange, Error> first =
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 8.0F, 4));
    CY_REQUIRE(first.has_value());
    const Expected<TileChange, Error> second =
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{1, 0, 0}, 8.0F, 4));
    CY_REQUIRE(second.has_value());
    return mesh;
}

}  // namespace

CY_TEST_CASE("A* crosses a tile boundary and the funnel pulls the path straight") {
    NavMesh mesh = two_tile_mesh();
    // z = 3 is the middle of a 2 m cell row, deliberately: a straight line along z = 4 would lie
    // exactly on every shared edge of the grid, which is a degenerate funnel rather than a
    // straight one, and testing a tolerance is not testing the algorithm.
    const Vec3 start{1.0F, 0.0F, 3.0F};
    const Vec3 end{14.0F, 0.0F, 3.0F};
    PathCorridor corridor(allocator());
    const PathResult result =
        find_path(mesh, start, end, Vec3{2.0F, 2.0F, 2.0F}, PathFilter{}, corridor);

    CY_REQUIRE(result.found);
    CY_CHECK_FALSE(result.partial);
    CY_CHECK_FALSE(result.budget_exceeded);
    CY_CHECK_GT(corridor.size(), usize{3});

    Array<PathPoint> path(allocator());
    const Status straightened = straighten(mesh, corridor, start, end, path);
    CY_REQUIRE(straightened.has_value());
    // `navigation`: "start and target lie on a large flat region crossed by many polygons ... the
    // funnel SHALL reduce the path to a straight two-point line".
    CY_CHECK_EQ(path.size(), usize{2});
    CY_CHECK_NEAR(path[1].position.x, 14.0F, 0.001F);
}

CY_TEST_CASE("an unreachable target answers the closest reachable point, flagged partial") {
    // Two tiles with a gap between them: both endpoints are ON the mesh, and no route joins them.
    // That is `navigation`'s "no path exists" — distinct from a target that is off the mesh
    // entirely, which `find_nearest` refuses before a search begins.
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 8.0F, 4)).has_value());
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{2, 0, 0}, 8.0F, 4)).has_value());

    PathCorridor corridor(allocator());
    const PathResult result = find_path(mesh, Vec3{1.0F, 0.0F, 3.0F}, Vec3{22.0F, 0.0F, 3.0F},
                                        Vec3{2.0F, 2.0F, 2.0F}, PathFilter{}, corridor);

    CY_CHECK(result.found);
    CY_CHECK(result.partial);
    CY_CHECK_FALSE(corridor.empty());
    // The corridor ends in the tile the agent started in, at the polygon closest to the target.
    CY_CHECK_EQ(mesh.tile_coord(corridor.polys()[corridor.size() - 1].tile()).x, 0);
}

CY_TEST_CASE("a search that exceeds its node budget reports it and answers its best partial") {
    NavMesh mesh = two_tile_mesh();
    PathFilter filter;
    filter.node_budget = 2;
    PathCorridor corridor(allocator());
    const PathResult result = find_path(mesh, Vec3{1.0F, 0.0F, 1.0F}, Vec3{15.0F, 0.0F, 7.0F},
                                        Vec3{2.0F, 2.0F, 2.0F}, filter, corridor);

    CY_CHECK(result.budget_exceeded);
    CY_CHECK(result.partial);
    CY_CHECK_LE(result.nodes_expanded, 3U);
}

CY_TEST_CASE(
    "an area mask excludes a type entirely and a cost multiplier only prefers against it") {
    // Ground, then a band of water, then ground: both endpoints are on ground, so what the mask
    // and the multiplier change is the ROUTE and not whether the endpoints resolve.
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    constexpr AreaType kWater = 4;
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 8.0F, 4)).has_value());
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 1, 0}, 8.0F, 4, 0.0F, kWater))
            .has_value());
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 2, 0}, 8.0F, 4)).has_value());

    const Vec3 start{3.0F, 0.0F, 1.0F};
    const Vec3 end{3.0F, 0.0F, 21.0F};
    PathCorridor corridor(allocator());
    const PathResult through =
        find_path(mesh, start, end, Vec3{2.0F, 2.0F, 2.0F}, PathFilter{}, corridor);
    CY_CHECK(through.found);
    CY_CHECK_FALSE(through.partial);

    PathFilter masked;
    masked.areas = kAllAreas & ~area_bit(kWater);
    corridor.clear();
    const PathResult blocked =
        find_path(mesh, start, end, Vec3{2.0F, 2.0F, 2.0F}, masked, corridor);
    CY_CHECK(blocked.partial);

    // A multiplier is a preference, not an exclusion: the only route still goes through, and it
    // costs more than the same route did without the aversion.
    PathFilter expensive;
    expensive.costs.multiplier[kWater] = 20.0F;
    corridor.clear();
    const PathResult reluctant =
        find_path(mesh, start, end, Vec3{2.0F, 2.0F, 2.0F}, expensive, corridor);
    CY_CHECK(reluctant.found);
    CY_CHECK_FALSE(reluctant.partial);
    CY_CHECK_GT(reluctant.cost, through.cost);
}

CY_TEST_CASE("a link an agent lacks the capability for is not expanded") {
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    const Expected<TileChange, Error> island_a =
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 8.0F, 4));
    CY_REQUIRE(island_a.has_value());
    // Two tiles apart, so nothing connects them but the link.
    const Expected<TileChange, Error> island_b =
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{2, 0, 0}, 8.0F, 4));
    CY_REQUIRE(island_b.has_value());

    constexpr CapabilityMask kCanJump = CapabilityMask{1} << 2U;
    NavLink jump;
    jump.from = Vec3{7.0F, 0.0F, 4.0F};
    jump.to = Vec3{17.0F, 0.0F, 4.0F};
    jump.action = Name::intern("Jump");
    jump.requires_capabilities = kCanJump;
    jump.cost = 2.0F;
    const Expected<LinkId, Error> id = mesh.add_link(jump, Vec3{2.0F, 2.0F, 2.0F});
    CY_REQUIRE(id.has_value());

    PathFilter jumper;
    jumper.capabilities = kCanJump;
    PathCorridor corridor(allocator());
    const PathResult crossed = find_path(mesh, Vec3{1.0F, 0.0F, 4.0F}, Vec3{22.0F, 0.0F, 4.0F},
                                         Vec3{2.0F, 2.0F, 2.0F}, jumper, corridor);
    CY_CHECK(crossed.found);
    CY_CHECK_FALSE(crossed.partial);

    // The path names the link's endpoints with its action, which is how gameplay knows to jump.
    Array<PathPoint> path(allocator());
    const Status straightened =
        straighten(mesh, corridor, Vec3{1.0F, 0.0F, 4.0F}, Vec3{22.0F, 0.0F, 4.0F}, path);
    CY_REQUIRE(straightened.has_value());
    bool tagged = false;
    for (const PathPoint& point : path.span()) {
        tagged = tagged || (point.enters_link && point.action == Name::intern("Jump"));
    }
    CY_CHECK(tagged);

    PathFilter walker;
    walker.capabilities = kNoCapabilities;
    corridor.clear();
    const PathResult stopped = find_path(mesh, Vec3{1.0F, 0.0F, 4.0F}, Vec3{22.0F, 0.0F, 4.0F},
                                         Vec3{2.0F, 2.0F, 2.0F}, walker, corridor);
    CY_CHECK(stopped.partial);
}

CY_TEST_CASE("a corridor over a released tile stops being valid") {
    NavMesh mesh = two_tile_mesh();
    PathCorridor corridor(allocator());
    const PathResult result = find_path(mesh, Vec3{1.0F, 0.0F, 4.0F}, Vec3{14.0F, 0.0F, 4.0F},
                                        Vec3{2.0F, 2.0F, 2.0F}, PathFilter{}, corridor);
    CY_REQUIRE(result.found);
    CY_CHECK(corridor.valid_against(mesh));

    const Status removed = mesh.remove_tile(TileCoord{1, 0, 0});
    CY_REQUIRE(removed.has_value());
    // `navigation`: "the agent's path SHALL be invalidated cleanly and a repath triggered, rather
    // than dereferencing released data".
    CY_CHECK_FALSE(corridor.valid_against(mesh));
}

CY_TEST_CASE("simplification and corner rounding keep the path's endpoints") {
    NavMesh mesh = two_tile_mesh();
    Array<PathPoint> path(allocator());
    for (const Vec3 position : {Vec3{0.0F, 0.0F, 0.0F}, Vec3{4.0F, 0.0F, 0.02F},
                                Vec3{8.0F, 0.0F, 0.0F}, Vec3{8.0F, 0.0F, 8.0F}}) {
        PathPoint point;
        point.position = position;
        const Status pushed = path.push_back(point);
        CY_REQUIRE(pushed.has_value());
    }

    simplify(path, 0.1F);
    CY_CHECK_EQ(path.size(), usize{3});
    CY_CHECK_NEAR(path[0].position.x, 0.0F, 0.001F);
    CY_CHECK_NEAR(path[2].position.z, 8.0F, 0.001F);

    const Status rounded = round_corners(path, 1.0F, 2);
    CY_REQUIRE(rounded.has_value());
    CY_CHECK_GT(path.size(), usize{3});
    CY_CHECK_NEAR(path[0].position.x, 0.0F, 0.001F);
    CY_CHECK_NEAR(path[path.size() - 1].position.z, 8.0F, 0.001F);
}

CY_TEST_CASE("an async query delivers on a tick derived from its submission, in submission order") {
    NavMesh mesh = two_tile_mesh();
    PathQueue queue(allocator(), mesh, 2);

    const Expected<QueryId, Error> first =
        queue.submit(11, Vec3{1.0F, 0.0F, 1.0F}, Vec3{14.0F, 0.0F, 6.0F}, Vec3{2.0F, 2.0F, 2.0F},
                     PathFilter{}, 100);
    const Expected<QueryId, Error> second =
        queue.submit(22, Vec3{1.0F, 0.0F, 6.0F}, Vec3{14.0F, 0.0F, 1.0F}, Vec3{2.0F, 2.0F, 2.0F},
                     PathFilter{}, 100);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());

    CY_CHECK_EQ(queue.update(101), 0U);
    CY_CHECK_EQ(queue.state(*first), QueryState::Pending);
    CY_CHECK_EQ(queue.update(102), 2U);
    CY_REQUIRE_EQ(queue.completed().size(), usize{2});
    CY_CHECK_EQ(queue.completed()[0], *first);
    CY_CHECK_EQ(queue.completed()[1], *second);
    CY_CHECK_EQ(queue.owner(*second), u64{22});

    PathCorridor corridor(allocator());
    PathResult result;
    CY_CHECK(queue.consume(*first, corridor, result));
    CY_CHECK(result.found);
    CY_CHECK_EQ(queue.state(*first), QueryState::Consumed);
    CY_CHECK_FALSE(queue.consume(*first, corridor, result));
}

CY_TEST_CASE("a cancelled query never delivers") {
    NavMesh mesh = two_tile_mesh();
    PathQueue queue(allocator(), mesh, 1);
    const Expected<QueryId, Error> id =
        queue.submit(7, Vec3{1.0F, 0.0F, 1.0F}, Vec3{14.0F, 0.0F, 6.0F}, Vec3{2.0F, 2.0F, 2.0F},
                     PathFilter{}, 5);
    CY_REQUIRE(id.has_value());
    const Status cancelled = queue.cancel(*id);
    CY_REQUIRE(cancelled.has_value());
    CY_CHECK_EQ(queue.update(6), 0U);
    CY_CHECK_EQ(queue.state(*id), QueryState::Cancelled);
}
