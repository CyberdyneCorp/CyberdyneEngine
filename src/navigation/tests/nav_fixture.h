#pragma once
// A navigation mesh a test can build in three lines, and the one place that knows how a tile's
// arrays are laid out. M8.b section 6.
//
// Every suite here needs the same thing: a flat, walkable, correctly wound tile whose polygons are
// adjacent to each other and to the neighbouring tile. Writing that inline four times would make
// four subtly different meshes and the differences would be the first suspect in every failure.

#include <cy/core/memory/allocator.h>
#include <cy/navigation/build.h>
#include <cy/navigation/navmesh.h>
#include <cy/test/test.h>

namespace cy::navigation::testing {

/// A tile of `cells` x `cells` quads like `grid_tile` below, keeping only the quads for which
/// `walkable(row, column)` holds; a missing quad is a hole in the mesh, which is
/// how a test carves an obstacle or an L-shaped corridor out of an otherwise regular grid.
template <typename Walkable>
[[nodiscard]] inline NavTileData grid_tile_where(Allocator& allocator, TileCoord coord,
                                                 f32 tile_size, u32 cells, Walkable walkable,
                                                 f32 y = 0.0F,
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
            if (!walkable(row, column)) {
                continue;
            }
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

/// A tile of `cells` x `cells` quads spanning one `tile_size` square at `coord`, at height `y`.
///
/// Winding is counter-clockwise seen from above, which is what makes `NavMesh::add_tile`'s
/// opposite-winding edge match find the internal adjacency: the quad to the east traverses the
/// shared edge in the opposite direction.
[[nodiscard]] inline NavTileData grid_tile(Allocator& allocator, TileCoord coord, f32 tile_size,
                                           u32 cells, f32 y = 0.0F,
                                           AreaType area = kAreaGround) noexcept {
    return grid_tile_where(
        allocator, coord, tile_size, cells, [](u32, u32) noexcept { return true; }, y, area);
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

/// Source triangles for a bake, with the per-triangle classification arrays `build_tile` reads.
struct SourceGeometry {
    Array<Vec3> vertices;
    Array<u32> indices;
    Array<u8> layer;
    Array<u8> tag;
    Array<AreaType> area;

    explicit SourceGeometry(Allocator& allocator) noexcept
        : vertices(allocator),
          indices(allocator),
          layer(allocator),
          tag(allocator),
          area(allocator) {}

    [[nodiscard]] NavSourceGeometry geometry() const noexcept {
        return NavSourceGeometry{vertices.span(), indices.span(), layer.span(), tag.span(),
                                 area.span()};
    }

    void triangle(Vec3 a, Vec3 b, Vec3 c, AreaType kind = kAreaGround) noexcept {
        const u32 base = static_cast<u32>(vertices.size());
        CY_REQUIRE(vertices.push_back(a).has_value());
        CY_REQUIRE(vertices.push_back(b).has_value());
        CY_REQUIRE(vertices.push_back(c).has_value());
        for (u32 offset = 0; offset < 3; ++offset) {
            CY_REQUIRE(indices.push_back(base + offset).has_value());
        }
        CY_REQUIRE(layer.push_back(u8{0}).has_value());
        CY_REQUIRE(tag.push_back(u8{0}).has_value());
        CY_REQUIRE(area.push_back(kind).has_value());
    }

    /// Flat ground of `columns` x `rows` quads of `step` metres from (x0, z0), at height `y`,
    /// wound so the normal points up.
    void ground(f32 x0, f32 z0, u32 columns, u32 rows, f32 step, f32 y = 0.0F) noexcept {
        for (u32 row = 0; row < rows; ++row) {
            for (u32 column = 0; column < columns; ++column) {
                const f32 x = x0 + (static_cast<f32>(column) * step);
                const f32 z = z0 + (static_cast<f32>(row) * step);
                triangle(Vec3{x, y, z}, Vec3{x + step, y, z + step}, Vec3{x + step, y, z});
                triangle(Vec3{x, y, z}, Vec3{x, y, z + step}, Vec3{x + step, y, z + step});
            }
        }
    }
};

}  // namespace cy::navigation::testing
