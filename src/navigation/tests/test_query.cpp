// Path queries: A* over the polygon graph, the funnel, area preferences, budgets and partial
// results, off-mesh links, and the deterministic asynchronous queue. M8.b task 6.1.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/query.h>
#include <cy/test/test.h>

#include <algorithm>
#include <initializer_list>
#include <utility>

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

// --- Funnel regressions ------------------------------------------------------------------------
//
// The funnel once compared its sides with the opposite sign to `triarea2`'s "positive is left", so
// a path that crossed cells diagonally came back through every portal corner — a staircase —
// while a path along one cell row still came out straight, because there both signs agree. These
// cases are the diagonal and off-axis paths the row-only case above never exercised.

namespace {

/// Four tiles in a square, 16 m a side, of 2 m quads: a flat region crossed by many polygons.
[[nodiscard]] NavMesh open_field_mesh() noexcept {
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    for (const i32 z : {0, 1}) {
        for (const i32 x : {0, 1}) {
            CY_REQUIRE(mesh.add_tile(testing::grid_tile(allocator(), TileCoord{x, z, 0}, 8.0F, 4))
                           .has_value());
        }
    }
    return mesh;
}

/// One 8 m tile of 1 m quads keeping only the cells `walkable(row, column)` accepts.
template <typename Walkable>
[[nodiscard]] NavMesh carved_mesh(Walkable walkable) noexcept {
    NavMesh mesh(allocator(), Name::intern("test.nav"), 8.0F);
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile_where(allocator(), TileCoord{0, 0, 0}, 8.0F, 8, walkable))
            .has_value());
    return mesh;
}

/// Search and straighten from `start` to `end`; the corridor must be complete.
[[nodiscard]] Array<PathPoint> straight_path(const NavMesh& mesh, Vec3 start, Vec3 end) noexcept {
    PathCorridor corridor(allocator());
    const PathResult result =
        find_path(mesh, start, end, Vec3{0.5F, 2.0F, 0.5F}, PathFilter{}, corridor);
    CY_REQUIRE(result.found);
    CY_REQUIRE_FALSE(result.partial);
    Array<PathPoint> path(allocator());
    CY_REQUIRE(straighten(mesh, corridor, start, end, path).has_value());
    return path;
}

/// Straighten over the corridor of every polygon the segment from `start` to `end` crosses, in
/// order — the corridor that contains the straight line, whichever one a search would prefer. The
/// polygons are `open_field_mesh`'s 2 m cells, found at the midpoint between each pair of
/// consecutive grid-line crossings.
[[nodiscard]] Array<PathPoint> path_along(const NavMesh& mesh, Vec3 start, Vec3 end) noexcept {
    constexpr f32 kCell = 2.0F;
    constexpr u32 kLines = 8;
    Array<f32> crossings(allocator());
    CY_REQUIRE(crossings.push_back(0.0F).has_value());
    CY_REQUIRE(crossings.push_back(1.0F).has_value());
    for (u32 line = 1; line < kLines; ++line) {
        const f32 at = static_cast<f32>(line) * kCell;
        for (const auto& [from, to] : {std::pair{start.x, end.x}, std::pair{start.z, end.z}}) {
            if ((at - from) * (at - to) < 0.0F) {
                CY_REQUIRE(crossings.push_back((at - from) / (to - from)).has_value());
            }
        }
    }
    std::ranges::sort(crossings);

    PathCorridor corridor(allocator());
    for (usize index = 1; index < crossings.size(); ++index) {
        const f32 t = 0.5F * (crossings[index - 1] + crossings[index]);
        Vec3 on;
        const PolyRef poly =
            mesh.find_nearest(start + ((end - start) * t), Vec3{0.01F, 1.0F, 0.01F}, kAllAreas, on);
        CY_REQUIRE(poly.valid());
        CY_REQUIRE(corridor.push(poly, kInvalidLink).has_value());
    }
    Array<PathPoint> path(allocator());
    CY_REQUIRE(straighten(mesh, corridor, start, end, path).has_value());
    return path;
}

/// The path is exactly `expected`, point for point, in the XZ plane.
void check_path(const Array<PathPoint>& path, std::initializer_list<Vec3> expected) noexcept {
    CY_REQUIRE_EQ(path.size(), expected.size());
    if (path.size() != expected.size()) {
        return;
    }
    usize index = 0;
    for (const Vec3 point : expected) {
        CY_CHECK_NEAR(path[index].position.x, point.x, 0.001F);
        CY_CHECK_NEAR(path[index].position.z, point.z, 0.001F);
        ++index;
    }
}

}  // namespace

CY_TEST_CASE("the funnel pulls a diagonal or off-axis path across a quad grid taut") {
    // Each pair crosses many cells without passing through a grid vertex. Over the corridor of the
    // cells the segment crosses, the taut path is the two endpoints and nothing else; both
    // directions are checked, because a sign error on one side of the funnel only shows when the
    // path turns toward that side. The corridor is built rather than searched for: on a grid, A*
    // may return a different staircase of cells, and over THAT corridor the taut path legitimately
    // bends.
    struct Case {
        const char* name = nullptr;
        Vec3 start;
        Vec3 end;
    };
    const Case cases[] = {
        {"diagonal, +x +z", Vec3{3.0F, 0.0F, 2.5F}, Vec3{13.0F, 0.0F, 12.5F}},
        {"diagonal, -x +z", Vec3{13.0F, 0.0F, 2.5F}, Vec3{3.0F, 0.0F, 12.5F}},
        {"shallow, +x +z", Vec3{3.0F, 0.0F, 3.0F}, Vec3{13.0F, 0.0F, 11.0F}},
        {"steep, -x +z", Vec3{15.0F, 0.0F, 1.0F}, Vec3{9.5F, 0.0F, 15.0F}},
        {"shallow, +x -z", Vec3{1.0F, 0.0F, 13.0F}, Vec3{15.0F, 0.0F, 5.0F}},
        {"along a row", Vec3{1.0F, 0.0F, 3.0F}, Vec3{15.0F, 0.0F, 3.0F}},
        {"along a column", Vec3{5.0F, 0.0F, 1.0F}, Vec3{5.0F, 0.0F, 15.0F}},
    };
    const NavMesh mesh = open_field_mesh();
    for (const Case& test : cases) {
        CY_TEST_SUBCASE(test.name) {
            check_path(path_along(mesh, test.start, test.end), {test.start, test.end});
            check_path(path_along(mesh, test.end, test.start), {test.end, test.start});
        }
    }
}

CY_TEST_CASE("a diagonal through grid vertices comes back on the straight line") {
    // (3, 3) to (13, 13) is the report's own case: it passes exactly through the vertices at
    // (4, 4), (6, 6) ... which are portal endpoints, so the funnel may keep a collinear point —
    // but every point must lie ON the line, never on a staircase beside it.
    const NavMesh mesh = open_field_mesh();
    const Vec3 start{3.0F, 0.0F, 3.0F};
    const Vec3 end{13.0F, 0.0F, 13.0F};
    const Array<PathPoint> path = straight_path(mesh, start, end);
    CY_REQUIRE(path.size() >= usize{2});
    CY_CHECK_NEAR(path[0].position.x, start.x, 0.001F);
    CY_CHECK_NEAR(path[path.size() - 1].position.x, end.x, 0.001F);
    for (const PathPoint& point : path.span()) {
        CY_CHECK_NEAR(point.position.x, point.position.z, 0.001F);
    }
}

// The two cases below carve one-cell-wide lanes, so the corridor A* returns is the only one there
// is and the expected corners are a property of the geometry, not of the search's tie-breaks.

CY_TEST_CASE("a path around an obstacle bends only at the obstacle's corners") {
    // A U of lanes — the columns x in [0, 1] and x in [4, 5] joined by the row z in [0, 1], all
    // within z < 5 — around the solid block x in [1, 4], z in [1, 5]. From the top of one arm to
    // the top of the other, the taut path drops to the block's two lower corners and nowhere else.
    const NavMesh mesh = carved_mesh([](u32 row, u32 column) noexcept {
        return row < 5 && column < 5 && (column == 0 || column == 4 || row == 0);
    });
    const Vec3 west_arm{0.5F, 0.0F, 4.5F};
    const Vec3 east_arm{4.5F, 0.0F, 4.5F};
    const Vec3 west_corner{1.0F, 0.0F, 1.0F};
    const Vec3 east_corner{4.0F, 0.0F, 1.0F};

    check_path(straight_path(mesh, west_arm, east_arm),
               {west_arm, west_corner, east_corner, east_arm});
    check_path(straight_path(mesh, east_arm, west_arm),
               {east_arm, east_corner, west_corner, west_arm});
}

CY_TEST_CASE("a path through an L-shaped corridor touches exactly the inner corner") {
    // Walkable: the column x in [0, 1] and the row z in [0, 1]. From the far end of one arm to the
    // far end of the other, the taut path turns once, at the inner corner (1, 1).
    const NavMesh mesh =
        carved_mesh([](u32 row, u32 column) noexcept { return column == 0 || row == 0; });
    const Vec3 column_end{0.5F, 0.0F, 7.5F};
    const Vec3 row_end{7.5F, 0.0F, 0.5F};
    const Vec3 corner{1.0F, 0.0F, 1.0F};

    check_path(straight_path(mesh, column_end, row_end), {column_end, corner, row_end});
    check_path(straight_path(mesh, row_end, column_end), {row_end, corner, column_end});
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
