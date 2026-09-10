// Chunked tilemaps, the grid shapes, and the terrain solver. M8.b task 9.5.

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/2d/tilemap.h>
#include <cy/test/test.h>

#include <cmath>

using namespace cy;
using namespace cy::rendering2d;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

[[nodiscard]] Tile tile_of(u16 id, u8 a, u8 b, u8 c, u8 d) noexcept {
    Tile tile;
    tile.id = id;
    tile.source = Rect2D{static_cast<f32>(id) * 16.0F, 0.0F, 16.0F, 16.0F};
    tile.corners[0] = a;
    tile.corners[1] = b;
    tile.corners[2] = c;
    tile.corners[3] = d;
    return tile;
}

/// A tileset with the four corner combinations a one-terrain autotile needs, plus a plain tile.
[[nodiscard]] Status build_tileset(Tileset& tileset) noexcept {
    if (Status added = tileset.add(tile_of(1, 0, 0, 0, 0)); !added) {  // empty
        return added;
    }
    if (Status added = tileset.add(tile_of(2, 1, 1, 1, 1)); !added) {  // full
        return added;
    }
    if (Status added = tileset.add(tile_of(3, 0, 1, 1, 0)); !added) {  // right edge
        return added;
    }
    if (Status added = tileset.add(tile_of(4, 1, 0, 0, 1)); !added) {  // left edge
        return added;
    }
    return tileset.add(tile_of(5, 1, 1, 0, 0));  // top edge
}

}  // namespace

CY_TEST_CASE("tilemap: storage is sparse — a chunk exists only where cells do") {
    TileLayer layer(allocator(), GridShape::Square);
    CY_CHECK_EQ(layer.chunk_count(), 0U);

    Cell cell;
    cell.tile = 2;
    CY_REQUIRE(layer.set_cell(0, 0, cell).has_value());
    CY_CHECK_EQ(layer.chunk_count(), 1U);

    // A cell in the same chunk adds no chunk; one a hundred chunks away adds exactly one.
    CY_REQUIRE(layer.set_cell(5, 5, cell).has_value());
    CY_CHECK_EQ(layer.chunk_count(), 1U);
    CY_REQUIRE(layer.set_cell(1600, 1600, cell).has_value());
    CY_CHECK_EQ(layer.chunk_count(), 2U);

    CY_REQUIRE(layer.cell(0, 0) != nullptr);
    CY_CHECK_EQ(layer.cell(0, 0)->tile, 2U);
    CY_CHECK_EQ(layer.cell(3, 3), nullptr);
}

CY_TEST_CASE("tilemap: one cell edit rebuilds one chunk") {
    // "WHEN one cell changes THEN only its chunk's geometry, collision, and navigation SHALL be
    // rebuilt."
    Tileset tileset(allocator());
    CY_REQUIRE(build_tileset(tileset).has_value());
    TileLayer layer(allocator(), GridShape::Square);
    Layer2D layer_settings;

    Cell cell;
    cell.tile = 2;
    CY_REQUIRE(layer.set_cell(0, 0, cell).has_value());
    CY_REQUIRE(layer.set_cell(100, 100, cell).has_value());
    CY_REQUIRE(layer.set_cell(200, 200, cell).has_value());
    CY_CHECK_EQ(layer.chunk_count(), 3U);

    Array<Draw2D> draws(allocator());
    Array<ChunkBuild> builds(allocator());
    TilemapReport report;
    CY_REQUIRE(layer.rebuild(tileset, layer_settings, 0.0F, draws, builds, report).has_value());
    CY_CHECK_EQ(report.rebuilt_geometry, 3U);
    CY_CHECK_EQ(report.cells, 3U);

    // Nothing changed: nothing is rebuilt.
    draws.clear();
    TilemapReport idle;
    CY_REQUIRE(layer.rebuild(tileset, layer_settings, 0.016F, draws, builds, idle).has_value());
    CY_CHECK_EQ(idle.rebuilt_geometry, 0U);
    CY_CHECK_EQ(idle.cells, 0U);

    // ONE cell changed: ONE chunk rebuilt.
    cell.tile = 3;
    CY_REQUIRE(layer.set_cell(100, 100, cell).has_value());
    draws.clear();
    TilemapReport partial;
    CY_REQUIRE(layer.rebuild(tileset, layer_settings, 0.016F, draws, builds, partial).has_value());
    CY_CHECK_EQ(partial.rebuilt_geometry, 1U);
    CY_CHECK_EQ(partial.cells, 1U);
}

CY_TEST_CASE("tilemap: an animated tile re-submits without rebuilding geometry") {
    // "WHEN a tile declares animation frames THEN its chunk SHALL be re-submitted with an updated
    // frame index without rebuilding geometry." TWO FLAGS, and this is why.
    Tileset tileset(allocator());
    Tile torch = tile_of(10, 0, 0, 0, 0);
    torch.animation_first = 10;
    torch.animation_frames = 3;
    torch.animation_seconds = 0.1F;
    CY_REQUIRE(tileset.add(torch).has_value());
    CY_REQUIRE(tileset.add(tile_of(11, 0, 0, 0, 0)).has_value());
    CY_REQUIRE(tileset.add(tile_of(12, 0, 0, 0, 0)).has_value());

    TileLayer layer(allocator(), GridShape::Square);
    Layer2D settings;
    Cell cell;
    cell.tile = 10;
    CY_REQUIRE(layer.set_cell(1, 1, cell).has_value());

    Array<Draw2D> draws(allocator());
    Array<ChunkBuild> builds(allocator());
    TilemapReport report;
    CY_REQUIRE(layer.rebuild(tileset, settings, 0.0F, draws, builds, report).has_value());
    CY_REQUIRE_EQ(draws.size(), 1U);
    const f32 first_frame_u = draws[0].source.x;

    // Only the instance data is dirty: the geometry is not rebuilt, and the frame moved.
    layer.touch_animation(chunk_of(1, 1));
    CY_CHECK(layer.dirty_of(chunk_of(1, 1)).instances);
    CY_CHECK_FALSE(layer.dirty_of(chunk_of(1, 1)).geometry);

    draws.clear();
    TilemapReport animated;
    CY_REQUIRE(layer.rebuild(tileset, settings, 0.15F, draws, builds, animated).has_value());
    CY_CHECK_EQ(animated.rebuilt_geometry, 0U);
    CY_CHECK_EQ(animated.rebuilt_instances, 1U);
    CY_REQUIRE_EQ(draws.size(), 1U);
    CY_CHECK_NE(draws[0].source.x, first_frame_u);
}

CY_TEST_CASE("tilemap: the four grid shapes place a cell where their geometry says") {
    const Vec2 size{32.0F, 16.0F};
    // Square: a step in x is a tile right.
    CY_CHECK_NEAR(cell_origin(GridShape::Square, size, 2, 3).x, 64.0F, 1e-4F);
    CY_CHECK_NEAR(cell_origin(GridShape::Square, size, 2, 3).y, 48.0F, 1e-4F);

    // Isometric: a step in x moves half right and half down; a step in y moves half left and half
    // down — the diamond that makes an isometric map.
    const Vec2 iso = cell_origin(GridShape::Isometric, size, 1, 0);
    const Vec2 iso_y = cell_origin(GridShape::Isometric, size, 0, 1);
    CY_CHECK_NEAR(iso.x, 16.0F, 1e-4F);
    CY_CHECK_NEAR(iso.y, 8.0F, 1e-4F);
    CY_CHECK_NEAR(iso_y.x, -16.0F, 1e-4F);
    CY_CHECK_NEAR(iso_y.y, 8.0F, 1e-4F);

    // Hexagonal, flat-topped: columns three quarters apart, every other column dropped by half.
    CY_CHECK_NEAR(cell_origin(GridShape::HexagonalFlatTop, size, 1, 0).x, 24.0F, 1e-4F);
    CY_CHECK_NEAR(cell_origin(GridShape::HexagonalFlatTop, size, 1, 0).y, 8.0F, 1e-4F);
    // Half-offset square: every other ROW is offset instead.
    CY_CHECK_NEAR(cell_origin(GridShape::HalfOffsetSquare, size, 0, 1).x, 16.0F, 1e-4F);
    CY_CHECK_NEAR(cell_origin(GridShape::HalfOffsetSquare, size, 0, 0).x, 0.0F, 1e-4F);
}

CY_TEST_CASE("tilemap: negative coordinates land in the chunk below and to the left") {
    // The arithmetic that a plain integer division gets wrong, and the reason `floor_div` exists.
    CY_CHECK_EQ(chunk_of(-1, -1).x, -1);
    CY_CHECK_EQ(chunk_of(-1, -1).y, -1);
    CY_CHECK_EQ(chunk_of(-16, 0).x, -1);
    CY_CHECK_EQ(chunk_of(-17, 0).x, -2);

    TileLayer layer(allocator(), GridShape::Square);
    Cell cell;
    cell.tile = 1;
    CY_REQUIRE(layer.set_cell(-5, -5, cell).has_value());
    CY_REQUIRE(layer.cell(-5, -5) != nullptr);
    CY_CHECK_EQ(layer.cell(-5, -5)->tile, 1U);
}

CY_TEST_CASE("tilemap: a runtime override replaces a cell and records that it did") {
    // "Runtime cell overrides — per-cell modifications without editing the tileset."
    TileLayer layer(allocator(), GridShape::Square);
    Cell authored;
    authored.tile = 2;
    CY_REQUIRE(layer.set_cell(4, 4, authored).has_value());
    CY_CHECK_FALSE(layer.cell(4, 4)->overridden);

    Cell replaced;
    replaced.tile = 3;
    CY_REQUIRE(layer.override_cell(4, 4, replaced).has_value());
    CY_CHECK_EQ(layer.cell(4, 4)->tile, 3U);
    CY_CHECK(layer.cell(4, 4)->overridden);
}

CY_TEST_CASE("tilemap: the terrain solver matches corners exactly, or says it could not") {
    // "WHEN cells are painted with a terrain in corner-and-edge mode THEN the solver SHALL choose
    // tiles so all shared corners and edges match."
    Tileset tileset(allocator());
    CY_REQUIRE(build_tileset(tileset).has_value());

    bool exact = false;
    TerrainCorners full;
    full.corners[0] = 1;
    full.corners[1] = 1;
    full.corners[2] = 1;
    full.corners[3] = 1;
    CY_CHECK_EQ(solve_terrain(tileset, full, exact), 2U);
    CY_CHECK(exact);

    TerrainCorners right_edge;
    right_edge.corners[1] = 1;
    right_edge.corners[2] = 1;
    CY_CHECK_EQ(solve_terrain(tileset, right_edge, exact), 3U);
    CY_CHECK(exact);

    // A combination the tileset does not have: the closest tile, and `exact` SAYS SO. A tileset
    // with a hole in it should produce a visible seam rather than nothing.
    TerrainCorners missing;
    missing.corners[0] = 1;
    missing.corners[2] = 1;
    const u16 chosen = solve_terrain(tileset, missing, exact);
    CY_CHECK_FALSE(exact);
    CY_CHECK_NE(chosen, 0U);
}

CY_TEST_CASE("tilemap: painting a terrain resolves the cells and their neighbours") {
    Tileset tileset(allocator());
    CY_REQUIRE(build_tileset(tileset).has_value());
    TileLayer layer(allocator(), GridShape::Square);

    CY_REQUIRE(paint_terrain(layer, tileset, 0, 0, 3, 3, 1).has_value());
    // The middle of the brush is fully inside the terrain.
    CY_REQUIRE(layer.cell(1, 1) != nullptr);
    CY_CHECK_EQ(layer.cell(1, 1)->tile, 2U);
    // Every painted cell has a tile, and the solve ran over the margin too — so a cell just outside
    // the brush that shares a corner was considered rather than left mismatched.
    CY_REQUIRE(layer.cell(0, 0) != nullptr);
    CY_CHECK_NE(layer.cell(0, 0)->tile, 0U);

    // A brush with no extent is refused rather than painting nothing quietly.
    CY_CHECK_FALSE(paint_terrain(layer, tileset, 0, 0, 0, 3, 1).has_value());
}
