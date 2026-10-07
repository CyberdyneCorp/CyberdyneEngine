// SPDX-License-Identifier: MIT
#pragma once
// Shared by the movement suites: a navigation mesh built by src/navigation/tests/nav_fixture.h and
// converted into a Fixed world, and the spelling of Fixed values a reader can check by hand.

#include <cy/core/memory/system_allocator.h>
#include <cy/movement/nav_mesh.h>
#include <cy/test/test.h>

#include "nav_fixture.h"

namespace cy::movement_test {

using detmath::Fixed;
using detmath::FixedVec2;

[[nodiscard]] inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// `value` metres.
[[nodiscard]] inline Fixed metres(i32 value) noexcept {
    return Fixed::from_int(value);
}

/// `numerator / 2^shift` metres, exactly.
[[nodiscard]] inline Fixed fraction(i64 numerator, int shift) noexcept {
    return Fixed::from_raw((numerator << Fixed::kFractionBits) >> shift);
}

[[nodiscard]] inline FixedVec2 at(i32 x, i32 z) noexcept {
    return FixedVec2{metres(x), metres(z)};
}

/// A `tile_size`-metre tile of `cells` x `cells` unit quads, keeping the quads `walkable` admits.
template <typename Walkable>
[[nodiscard]] inline navigation::NavMesh grid_mesh(f32 tile_size, u32 cells,
                                                   Walkable walkable) noexcept {
    navigation::NavMesh mesh(allocator(), Name::intern("movement.test"), tile_size);
    Expected<navigation::TileChange, Error> published =
        mesh.add_tile(navigation::testing::grid_tile_where(
            allocator(), navigation::TileCoord{0, 0, 0}, tile_size, cells, walkable));
    CY_REQUIRE(published.has_value());
    return mesh;
}

/// An open `cells` x `cells` grid of 1 m quads.
[[nodiscard]] inline navigation::NavMesh open_mesh(u32 cells) noexcept {
    return grid_mesh(static_cast<f32>(cells), cells, [](u32, u32) noexcept { return true; });
}

/// `source`, converted.
[[nodiscard]] inline movement::FixedNavMesh converted(const navigation::NavMesh& source) noexcept {
    movement::FixedNavMesh mesh(allocator());
    CY_REQUIRE(mesh.convert(source).has_value());
    return mesh;
}

}  // namespace cy::movement_test
