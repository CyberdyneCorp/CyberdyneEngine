#pragma once
// The terrain the suites are written against.
//
// One layout, one generator shape and a few tile builders. They are deliberately SMALL — a 256 m
// tile of 64 quads is 4 m per sample — because every claim in these suites is about a rule rather
// than about a size, and a fixture that cooked a square kilometre would make the unit suite an
// integration one without testing anything more.

#include <cy/core/memory/system_allocator.h>
#include <cy/terrain/collision.h>
#include <cy/terrain/deform.h>
#include <cy/terrain/material.h>
#include <cy/terrain/meshing.h>
#include <cy/terrain/stack.h>
#include <cy/terrain/surface.h>
#include <cy/terrain/tile.h>
#include <cy/world/coordinates.h>

namespace cy::terrain::test {

[[nodiscard]] inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A planar grid of 256 m tiles at the origin, four levels at a ratio of two.
[[nodiscard]] inline TileLayout layout() noexcept {
    TileLayout value;
    value.scheme = CoordinateScheme::PlanarGrid;
    value.terrain = 1;
    value.tile_metres = 256.0F;
    value.levels = 4;
    value.level_ratio = 2;
    value.height_min = -256.0F;
    value.height_max = 1792.0F;
    return value;
}

/// The partition src/world/'s own fixtures use, so a cell footprint computed here is the one that
/// module would compute.
[[nodiscard]] inline world::PartitionConfig partition() noexcept {
    world::PartitionConfig config;
    config.partition = 1;
    config.base_cell_size = 128.0F;
    config.levels = 3;
    config.level_ratio = 4;
    return config;
}

/// A tile whose height is a declared function of the world position, so a test can state the
/// expected answer without consulting the thing it is testing.
template <typename Height>
[[nodiscard]] inline TerrainTile make_tile(const TileLayout& shape, const TileCoord& coord,
                                           Height&& height) noexcept {
    Expected<TerrainTile, Error> made = TerrainTile::create(allocator(), shape, coord, 0.0F);
    CY_ASSERT(made.has_value());
    TerrainTile tile = std::move(made.value());
    for (u32 j = 0; j < kTileVerts; ++j) {
        for (u32 i = 0; i < kTileVerts; ++i) {
            const TerrainPoint at = sample_position(shape, coord, i, j);
            tile.set_stored(i, j, quantise_height(shape, height(at.x, at.z)));
        }
    }
    tile.refresh_extent(shape);
    return tile;
}

/// A plane rising one metre every eight metres of x. Its slope and its height at a position are
/// arithmetic a test can do in its own head.
[[nodiscard]] inline f32 ramp(f64 x, f64 z) noexcept {
    (void)z;
    return static_cast<f32>(x) * 0.125F;
}

[[nodiscard]] inline f32 flat(f64 x, f64 z) noexcept {
    (void)x;
    (void)z;
    return 100.0F;
}

}  // namespace cy::terrain::test
