// SPDX-License-Identifier: MIT
// PATH QUERIES IN A FIXED WORLD. Task 6.2, design §9.1: A* with Fixed g-costs and a
// wide-square-root heuristic, ties by polygon index; the funnel on exact cross-product signs; path
// following that arrives without overshooting.

#include <cy/movement/path.h>

#include "movement_fixture.h"

namespace {

using cy::Array;
using cy::u32;
using cy::movement::FixedNavMesh;
using cy::movement::FixedPathResult;
using cy::movement::FixedPathSearch;
using cy::movement::FixedPolyIndex;
using namespace cy::movement_test;

/// An 8 x 8 grid with a wall at column 4, rows 0 to 6: the way round is through row 7.
[[nodiscard]] FixedNavMesh walled() noexcept {
    return converted(
        grid_mesh(8.0F, 8, [](u32 row, u32 column) noexcept { return column != 4 || row > 6; }));
}

}  // namespace

CY_TEST_CASE("movement path: along an open row the path is the straight line") {
    const FixedNavMesh mesh = converted(open_mesh(8));
    FixedPathSearch search(allocator());
    Array<FixedPolyIndex> corridor(allocator());
    Array<FixedVec2> points(allocator());
    const FixedVec2 start{fraction(1, 1), fraction(1, 1)};
    const FixedVec2 goal{fraction(15, 1), fraction(1, 1)};
    const FixedPathResult result =
        cy::movement::find_path(search, mesh, start, goal, 512, corridor, points);
    CY_CHECK(result.found);
    CY_CHECK_FALSE(result.partial);
    CY_CHECK_EQ(corridor.size(), 8U);
    CY_CHECK_EQ(corridor[0], 0U);
    CY_CHECK_EQ(corridor[corridor.size() - 1], 7U);
    CY_CHECK(result.cost == metres(7));
    // Taut: nothing between the ends, because the funnel never closed.
    CY_REQUIRE_EQ(points.size(), 2U);
    CY_CHECK(points[0] == start);
    CY_CHECK(points[1] == goal);
}

CY_TEST_CASE("movement path: around a wall the path turns exactly at the wall's end") {
    const FixedNavMesh mesh = walled();
    FixedPathSearch search(allocator());
    Array<FixedPolyIndex> corridor(allocator());
    Array<FixedVec2> points(allocator());
    const FixedVec2 start{fraction(5, 1), fraction(1, 1)};  // (2.5, 0.5), left of the wall
    const FixedVec2 goal{fraction(13, 1), fraction(1, 1)};  // (6.5, 0.5), right of it
    const FixedPathResult result =
        cy::movement::find_path(search, mesh, start, goal, 512, corridor, points);
    CY_REQUIRE(result.found);
    // The corridor runs from (2.5, 0.5) east into column 3, up it beside the wall, across the gap
    // in row 7 and down column 5. The taut string inside THAT corridor bends at the corner of
    // column 3 it first meets, at both corners of the gap, and at the matching corner of column 5:
    // every bend is a corridor vertex, decided by an exact cross-product sign.
    CY_REQUIRE_EQ(points.size(), 6U);
    CY_CHECK(points[0] == start);
    CY_CHECK(points[1] == at(3, 1));
    CY_CHECK(points[2] == at(4, 7));
    CY_CHECK(points[3] == at(5, 7));
    CY_CHECK(points[4] == at(6, 1));
    CY_CHECK(points[5] == goal);
    CY_CHECK(result.cost > metres(12));
}

CY_TEST_CASE("movement path: a blocked polygon is never entered") {
    FixedNavMesh mesh = walled();
    FixedPathSearch search(allocator());
    Array<FixedPolyIndex> corridor(allocator());
    // Block the gap: row 7, column 4 — the only polygon of column 4.
    const FixedPolyIndex gap = mesh.locate(FixedVec2{fraction(9, 1), fraction(15, 1)});
    mesh.set_blocked(gap, true);
    const FixedPathResult result =
        search.find_corridor(mesh, FixedVec2{fraction(5, 1), fraction(1, 1)},
                             FixedVec2{fraction(13, 1), fraction(1, 1)}, 512, corridor);
    CY_CHECK_FALSE(result.found);
    CY_CHECK(result.partial);
    for (const FixedPolyIndex poly : corridor) {
        CY_CHECK_NE(poly, gap);
    }
    // The partial corridor ends at the reachable polygon nearest the goal: beside the wall.
    const FixedNavMesh& view = mesh;
    CY_CHECK(view.poly(corridor[corridor.size() - 1]).centre.x == fraction(7, 1));
}

CY_TEST_CASE("movement path: the node budget stops a search and says so") {
    const FixedNavMesh mesh = walled();
    FixedPathSearch search(allocator());
    Array<FixedPolyIndex> corridor(allocator());
    const FixedPathResult result =
        search.find_corridor(mesh, FixedVec2{fraction(5, 1), fraction(1, 1)},
                             FixedVec2{fraction(13, 1), fraction(1, 1)}, 4, corridor);
    CY_CHECK(result.budget_exceeded);
    CY_CHECK_FALSE(result.found);
    CY_CHECK_EQ(result.nodes_expanded, 4U);
    CY_CHECK_FALSE(corridor.empty());
}

CY_TEST_CASE("movement path: equal-cost corridors resolve the same way every time") {
    // From one corner of an open square to the opposite, every monotone staircase costs the same in
    // centre-to-centre steps; the (f, index) order picks one, and picks it again.
    const FixedNavMesh mesh = converted(open_mesh(6));
    FixedPathSearch search(allocator());
    Array<FixedPolyIndex> first(allocator());
    Array<FixedPolyIndex> second(allocator());
    const FixedVec2 start{fraction(1, 1), fraction(1, 1)};
    const FixedVec2 goal{fraction(11, 1), fraction(11, 1)};
    (void)search.find_corridor(mesh, start, goal, 512, first);
    FixedPathSearch fresh(allocator());
    (void)fresh.find_corridor(mesh, start, goal, 512, second);
    CY_REQUIRE_EQ(first.size(), second.size());
    for (cy::usize i = 0; i < first.size(); ++i) {
        CY_CHECK_EQ(first[i], second[i]);
    }
}

CY_TEST_CASE("movement path: following arrives without overshooting, then stops") {
    Array<FixedVec2> path(allocator());
    CY_REQUIRE(path.push_back(at(0, 0)).has_value());
    CY_REQUIRE(path.push_back(at(10, 0)).has_value());
    const Fixed speed = metres(4);
    const Fixed arrival = fraction(1, 2);  // a quarter metre
    const Fixed rate = metres(60);
    u32 cursor = 0;

    // At the start: the first point is reached, so the unit heads for the second at full speed.
    const FixedVec2 full =
        cy::movement::follow_path(path.span(), at(0, 0), speed, arrival, rate, cursor);
    CY_CHECK_EQ(cursor, 1U);
    CY_CHECK(full == (FixedVec2{speed, Fixed::zero()}));

    // One sixtieth of a metre short: the velocity is what covers exactly that in one tick.
    const FixedVec2 near_end{Fixed::from_raw(metres(10).raw - (Fixed::kOneRaw / 60) - 1),
                             Fixed::zero()};
    const FixedVec2 last =
        cy::movement::follow_path(path.span(), near_end, speed, fraction(0, 0), rate, cursor);
    CY_CHECK(last.x < speed);
    CY_CHECK(last.x > Fixed::zero());

    // Within the arrival distance of the last point: done.
    const FixedVec2 done = cy::movement::follow_path(
        path.span(), FixedVec2{fraction(39, 2), Fixed::zero()}, speed, arrival, rate, cursor);
    CY_CHECK(done == FixedVec2::zero());
    CY_CHECK_EQ(cursor, 2U);
}
