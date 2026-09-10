#pragma once
// A navigation mesh a test can build in three lines, and the one place that knows how a tile's
// arrays are laid out. M8.b section 6.
//
// Every suite here needs the same thing: a flat, walkable, correctly wound tile whose polygons are
// adjacent to each other and to the neighbouring tile. Writing that inline four times would make
// four subtly different meshes and the differences would be the first suspect in every failure.

#include <cy/core/memory/allocator.h>
#include <cy/navigation/navmesh.h>
#include <cy/test/test.h>

namespace cy::navigation::testing {

/// A tile of `cells` x `cells` quads spanning one `tile_size` square at `coord`, at height `y`.
///
/// Winding is counter-clockwise seen from above, which is what makes `NavMesh::add_tile`'s
/// opposite-winding edge match find the internal adjacency: the quad to the east traverses the
/// shared edge in the opposite direction.
[[nodiscard]] inline NavTileData grid_tile(Allocator& allocator, TileCoord coord, f32 tile_size,
                                           u32 cells, f32 y = 0.0F,
                                           AreaType area = kAreaGround) noexcept {
    NavTileData data(allocator);
    data.coord = coord;

    const f32 step = tile_size / static_cast<f32>(cells);
    const f32 origin_x = static_cast<f32>(coord.x) * tile_size;
    const f32 origin_z = static_cast<f32>(coord.z) * tile_size;

    for (u32 row = 0; row <= cells; ++row) {
        for (u32 column = 0; column <= cells; ++column) {
            const Status pushed =
                data.vertices().push_back(Vec3{origin_x + (static_cast<f32>(column) * step), y,
                                               origin_z + (static_cast<f32>(row) * step)});
            CY_REQUIRE(pushed.has_value());
        }
    }

    const auto vertex_of = [cells](u32 row, u32 column) noexcept {
        return (row * (cells + 1)) + column;
    };
    for (u32 row = 0; row < cells; ++row) {
        for (u32 column = 0; column < cells; ++column) {
            NavPoly poly;
            poly.first_corner = static_cast<u32>(data.corners().size());
            poly.corner_count = 4;
            poly.area = area;
            poly.cost = 1.0F;
            const Status pushed = data.polys().push_back(poly);
            CY_REQUIRE(pushed.has_value());
            const u32 corner[4] = {vertex_of(row, column), vertex_of(row, column + 1),
                                   vertex_of(row + 1, column + 1), vertex_of(row + 1, column)};
            for (const u32 index : corner) {
                const Status added = data.corners().push_back(index);
                CY_REQUIRE(added.has_value());
            }
        }
    }
    data.finalise();
    return data;
}

/// One tile published into a fresh mesh. The default shape: 8 m square, four quads a side.
[[nodiscard]] inline NavMesh single_tile_mesh(Allocator& allocator, f32 tile_size = 8.0F,
                                              u32 cells = 4) noexcept {
    NavMesh mesh(allocator, Name::intern("test.nav"), tile_size);
    Expected<TileChange, Error> published =
        mesh.add_tile(grid_tile(allocator, TileCoord{0, 0, 0}, tile_size, cells));
    CY_REQUIRE(published.has_value());
    return mesh;
}

}  // namespace cy::navigation::testing
