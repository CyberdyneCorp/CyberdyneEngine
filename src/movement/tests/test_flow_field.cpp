// SPDX-License-Identifier: MIT
// FLOW FIELDS IN A FIXED WORLD. Task 6.2, design §9.1: Fixed integration costs, a (cost, cell)
// queue order, no corner cutting, and exact unit directions.

#include <cy/core/detmath/functions.h>
#include <cy/movement/flow_field.h>

#include "movement_fixture.h"

namespace {

using cy::u32;
using cy::movement::FixedFlowField;
using cy::movement::FixedNavMesh;
using namespace cy::movement_test;

/// The 8 x 8 grid with a wall at column 4, rows 0 to 6, as in the path suite.
[[nodiscard]] FixedNavMesh walled() noexcept {
    return converted(grid_mesh(
        8.0F, 8, [](u32 row, u32 column) noexcept { return !(column == 4 && row <= 6); }));
}

[[nodiscard]] FixedVec2 centre_of(cy::i32 x, cy::i32 z) noexcept {
    return FixedVec2{metres(x) + Fixed::half(), metres(z) + Fixed::half()};
}

}  // namespace

CY_TEST_CASE("movement flow field: costs integrate in Fixed and directions lead round the wall") {
    const FixedNavMesh mesh = walled();
    FixedFlowField field(allocator());
    const auto report = field.build(mesh, centre_of(6, 0), 0);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report->cells, 64U);
    CY_CHECK_EQ(report->walkable, 57U);
    CY_CHECK_EQ(report->reached, 57U);

    CY_CHECK(field.cost_at(centre_of(6, 0)) == Fixed::zero());
    CY_CHECK(field.cost_at(centre_of(5, 0)) == metres(1));
    CY_CHECK(field.cost_at(centre_of(7, 1)) == cy::detmath::sqrt(metres(2)));
    // Beside the destination, step onto it; below the gap, climb toward it; past it, come down.
    CY_CHECK(field.direction_at(centre_of(5, 0)) == at(1, 0));
    CY_CHECK(field.direction_at(centre_of(3, 0)) == at(0, 1));
    CY_CHECK(field.direction_at(centre_of(6, 5)) == at(0, -1));
    CY_CHECK(field.direction_at(centre_of(6, 0)) == FixedVec2::zero());
    // A diagonal is the correctly rounded square root of a half on each axis.
    const FixedVec2 diagonal = field.direction_at(centre_of(7, 1));
    CY_CHECK(diagonal.x == -cy::detmath::sqrt(Fixed::half()));
    CY_CHECK(diagonal.y == -cy::detmath::sqrt(Fixed::half()));
    // The field never cuts the wall's corner: from just left of the gap's row, the way is up.
    CY_CHECK(field.direction_at(centre_of(3, 6)) == at(0, 1));
}

CY_TEST_CASE("movement flow field: a blocked polygon cuts the field, and the build repeats") {
    FixedNavMesh mesh = walled();
    FixedFlowField open(allocator());
    FixedFlowField again(allocator());
    CY_REQUIRE(open.build(mesh, centre_of(6, 0), 0).has_value());
    CY_REQUIRE(again.build(mesh, centre_of(6, 0), 0).has_value());
    CY_CHECK_EQ(open.digest(), again.digest());

    mesh.set_blocked(mesh.locate(centre_of(4, 7)), true);
    FixedFlowField cut(allocator());
    const auto report = cut.build(mesh, centre_of(6, 0), 0);
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report->walkable, 56U);
    // Columns 0 to 3 are now unreachable: 32 cells.
    CY_CHECK_EQ(report->reached, 24U);
    CY_CHECK(cut.cost_at(centre_of(2, 2)) == Fixed::max());
    CY_CHECK(cut.direction_at(centre_of(2, 2)) == FixedVec2::zero());
    CY_CHECK_NE(cut.digest(), open.digest());
    CY_CHECK(cy::movement::follow_field(open, centre_of(5, 0), metres(3)) == at(3, 0));
}
