// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_FOG_GRID_H
#define CY_SAMPLE_RTS_API_FOG_GRID_H
// fog_grid.h — where the fog of war's grid lies, and what a viewer is shown of a unit by it.
//
// Header-only and free of the host, so integration.rts_map can hold the grid a map run builds to
// the map it was cooked from without a Swift toolchain or a device.
//
// THE GRID COVERS EVERYTHING THAT IS DRAWN. A run without a map draws the level's 32 m square, and
// the grid is that square. A run on the RTS test map draws the map's whole terrain, and the grid is
// sized from the cooked map's bounds (`map::map_bounds`), so every surface the frame fogs and every
// unit it draws has a cell. Ground off any grid is never explored: the frame's stage shades it as
// unexplored and the simulation answers `Unexplored` for it, never visible.

#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/gameplay/fog/fog_of_war.h>

#include <algorithm>
#include <cmath>

namespace sample::rts {

/// The grid's cell: a metre, as the level's grid always had.
inline constexpr cy::i64 kFogCellRaw = cy::gameplay::fog::kOneMetreRaw;

/// A grid of one-metre cells over `bounds` on the ground plane (x and z; y is ignored), its edges
/// on whole metres so every cell of the level's square is the cell it was. Empty bounds give an
/// empty description, which `FogOfWar::create` refuses.
[[nodiscard]] inline cy::gameplay::fog::FogGridDescription fog_grid_over(
    const cy::Aabb& bounds) noexcept {
    cy::gameplay::fog::FogGridDescription grid;
    if (!(bounds.max.x > bounds.min.x) || !(bounds.max.z > bounds.min.z)) {
        grid.width = 0;
        grid.height = 0;
        return grid;
    }
    const cy::f32 min_x = std::floor(bounds.min.x);
    const cy::f32 min_z = std::floor(bounds.min.z);
    grid.width = static_cast<cy::u32>(std::ceil(bounds.max.x) - min_x);
    grid.height = static_cast<cy::u32>(std::ceil(bounds.max.z) - min_z);
    grid.cell_size_raw = kFogCellRaw;
    grid.origin_x_raw = static_cast<cy::i64>(min_x) * kFogCellRaw;
    grid.origin_z_raw = static_cast<cy::i64>(min_z) * kFogCellRaw;
    return grid;
}

/// The ground the grid covers, in metres: its corner, and its far corner. y is zero.
[[nodiscard]] inline cy::Aabb fog_grid_bounds(
    const cy::gameplay::fog::FogGridDescription& grid) noexcept {
    const auto metres = [](cy::i64 raw) {
        return static_cast<cy::f32>(static_cast<cy::f64>(raw) /
                                    static_cast<cy::f64>(cy::gameplay::fog::kOneMetreRaw));
    };
    const cy::f32 min_x = metres(grid.origin_x_raw);
    const cy::f32 min_z = metres(grid.origin_z_raw);
    const cy::f32 cell = metres(grid.cell_size_raw);
    return cy::Aabb::from_min_max(cy::Vec3{min_x, 0.0F, min_z},
                                  cy::Vec3{min_x + (cell * static_cast<cy::f32>(grid.width)), 0.0F,
                                           min_z + (cell * static_cast<cy::f32>(grid.height))});
}

/// Metres as Q32.32, rounded to nearest: the one conversion a float position makes.
[[nodiscard]] inline cy::i64 fog_raw(cy::f32 metres) noexcept {
    return static_cast<cy::i64>(std::llround(static_cast<cy::f64>(metres) * 4294967296.0));
}

/// Whether `viewer` is shown a unit of `owner` standing at `at`: its own side's always; anyone
/// else's only while the cell under it is visible to `viewer`. Off the grid is never visible, so a
/// unit there is not drawn.
[[nodiscard]] inline bool shown_by_fog(const cy::gameplay::fog::FogOfWar& grid,
                                       cy::gameplay::TeamId viewer, cy::gameplay::TeamId owner,
                                       cy::Vec3 at) noexcept {
    if (owner == viewer) {
        return true;
    }
    return grid.at(viewer, fog_raw(at.x), fog_raw(at.z)) == cy::gameplay::fog::CellState::Visible;
}

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_FOG_GRID_H
