// Tiled hierarchical storage: identity, the replaceable coordinate scheme, the sparse hierarchy,
// the cook-time macro derivation, and the report of why a position is at the detail it is.
// M10 task 2.1, and `terrain`'s "Tiled hierarchical storage" and "Terrain hierarchical level of
// detail".
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL, AND WHAT THAT EXPOSED IN THE SUITE ITSELF.
// `TileLayout::signature()` had its `scheme` contribution removed. The scheme case AS FIRST WRITTEN
// still passed, because it compared two layouts that differed in the scheme AND in the cube face,
// and the face alone was enough to separate them. The case was narrowed to change only the scheme —
// the face is now its own assertion — and the same mutation went red on its first line with the two
// schemes naming identical tiles. It was then restored. Both halves are reported in this
// milestone's `verified_failing`.

#include <cy/test/test.h>

#include <string_view>

#include "fixtures.h"

namespace test = cy::terrain::test;
using namespace cy::terrain;

CY_TEST_CASE("a tile identity is stable, opaque, and derived from the coordinate and the level") {
    const TileLayout shape = test::layout();
    const TileId first = tile_id_of(shape, TileCoord{3, -7, 0});
    CY_REQUIRE(first.is_valid());
    CY_CHECK_EQ(first, tile_id_of(shape, TileCoord{3, -7, 0}));
    CY_CHECK_NE(first, tile_id_of(shape, TileCoord{3, -7, 1}));
    CY_CHECK_NE(first, tile_id_of(shape, TileCoord{-7, 3, 0}));

    // "Tile identity SHALL be stable ... so saves, patches, and streaming caches key on it": a
    // layout that differs anywhere names different tiles, which is what makes a stale cache report
    // a mismatch rather than answer for a different piece of the world.
    TileLayout wider = shape;
    wider.tile_metres = 512.0F;
    CY_CHECK_NE(first, tile_id_of(wider, TileCoord{3, -7, 0}));
}

CY_TEST_CASE("the coordinate scheme is replaceable, and changing it renames every tile") {
    const TileLayout planar = test::layout();
    // ONLY the scheme differs — not the face, not the tile size. A layout that folded the face in
    // and forgot the scheme would still pass a comparison that changed both, which is exactly the
    // kind of test that proves nothing.
    TileLayout planetary = planar;
    planetary.scheme = CoordinateScheme::CubeFaceQuadtree;

    CY_CHECK_NE(planar.signature(), planetary.signature());
    CY_CHECK_NE(tile_id_of(planar, TileCoord{1, 1, 0}), tile_id_of(planetary, TileCoord{1, 1, 0}));

    // And the face is its own axis: two faces of one cube are different places in the world.
    TileLayout other_face = planetary;
    other_face.face = 2;
    CY_CHECK_NE(planetary.signature(), other_face.signature());

    // And the QUERY path is unchanged by it: the same store, the same insert, the same answer,
    // under a scheme no function below tile.h names. That is the scenario's "without altering the
    // terrain query, collision, or rendering interfaces".
    TerrainStore store(test::allocator(), planetary);
    CY_REQUIRE(
        store.insert(test::make_tile(planetary, TileCoord{0, 0, 0}, test::flat)).has_value());
    TerrainDeltaStore deltas(test::allocator(), planetary);
    HeightfieldSource heights(store, &deltas);
    TerrainQuery query(test::allocator());
    CY_REQUIRE(query.add_source(heights).has_value());
    CY_CHECK_NEAR(query.sample(100.0, 100.0).height, 100.0F, 0.05F);
}

CY_TEST_CASE("there is no global grid: an absent region costs nothing and does not fault") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 0}, test::flat)).has_value());
    CY_REQUIRE(
        store.insert(test::make_tile(shape, TileCoord{10000, 10000, 0}, test::flat)).has_value());

    // Two tiles ten thousand tiles apart. A global heightmap covering both would be 2.5 million
    // kilometres of samples; this is two tiles.
    CY_CHECK_EQ(store.tile_count(), 2U);
    // Two tiles' worth of bytes and not one metre more: the cost is the tiles that exist, not the
    // rectangle that contains them.
    const cy::u64 one_tile = store.find(TileCoord{0, 0, 0})->bytes();
    CY_CHECK_EQ(store.bytes_resident(), one_tile * 2);
    CY_CHECK(store.finest_at(500000.0, 500000.0, 0) == nullptr);

    const TerrainDiagnostics diagnostics = store.diagnostics();
    CY_CHECK_EQ(diagnostics.tiles[0], 2U);
    CY_CHECK_EQ(diagnostics.min_tile_x, 0);
    CY_CHECK_EQ(diagnostics.max_tile_x, 10000);
}

CY_TEST_CASE("a macro tile is a decimation of its children, sample for sample") {
    const TileLayout shape = test::layout();
    TerrainTile children[4] = {
        test::make_tile(shape, TileCoord{0, 0, 0}, test::ramp),
        test::make_tile(shape, TileCoord{1, 0, 0}, test::ramp),
        test::make_tile(shape, TileCoord{0, 1, 0}, test::ramp),
        test::make_tile(shape, TileCoord{1, 1, 0}, test::ramp),
    };
    const TerrainTile* pointers[4] = {&children[0], &children[1], &children[2], &children[3]};

    cy::Expected<TerrainTile, cy::Error> coarse =
        derive_coarse(test::allocator(), shape, TileCoord{0, 0, 1},
                      cy::Span<const TerrainTile* const>(pointers, 4));
    CY_REQUIRE(coarse.has_value());

    // Decimation and not averaging: the coarse sample is the SAME STORED VALUE as the fine sample
    // standing at the same place, which is what makes a level transition have no seam to close.
    // Checked against the CHILD's stored value rather than against a recomputed height.
    for (cy::u32 j = 0; j <= 32; ++j) {
        for (cy::u32 i = 0; i <= 32; ++i) {
            CY_REQUIRE_EQ(coarse.value().stored(i, j), children[0].stored(i * 2, j * 2));
        }
    }
    CY_CHECK_EQ(coarse.value().stored(64, 64), children[3].stored(64, 64));
}

CY_TEST_CASE("detail coarsens with distance, and the store says why a position is coarse") {
    const TileLayout shape = test::layout();
    // Four metres per sample at level 0, eight at level 1, and so on.
    CY_CHECK_EQ(level_for_detail(shape, 10.0, 0.05F), 0U);
    CY_CHECK_GT(level_for_detail(shape, 10000.0, 0.05F), 0U);

    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 2}, test::flat)).has_value());

    // Nothing was asked for at level 0, so the cause is distance.
    CY_CHECK_EQ(store.explain(100.0, 100.0, 0).reason, DetailReason::Distance);
    CY_CHECK_EQ(store.explain(100.0, 100.0, 0).used.level, 2U);

    // A streamer that was refused says so, and the report changes without the data changing.
    CY_REQUIRE(store.note_wanted(TileCoord{0, 0, 0}, DetailReason::Budget).has_value());
    CY_CHECK_EQ(store.explain(100.0, 100.0, 0).reason, DetailReason::Budget);

    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 0}, test::flat)).has_value());
    const TileDetailReport resident = store.explain(100.0, 100.0, 0);
    CY_CHECK_EQ(resident.reason, DetailReason::Resident);
    CY_CHECK_EQ(resident.used.level, 0U);
    CY_CHECK_GT(resident.bytes, 0U);
}

CY_TEST_CASE(
    "a tile carries height, material, biome and holes, and eviction gives the bytes back") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    TerrainTile tile = test::make_tile(shape, TileCoord{0, 0, 0}, test::ramp);
    tile.texel(4, 4) = MaterialTexel{{3, 1, 0, 0}, {200, 55, 0, 0}};
    tile.biome[(4 * kTileTexels) + 4] = 7;
    tile.set_hole(10, 10, true);
    CY_REQUIRE(store.insert(std::move(tile)).has_value());

    const TerrainTile* held = store.find(TileCoord{0, 0, 0});
    CY_REQUIRE(held != nullptr);
    CY_CHECK_EQ(held->texel(4, 4).dominant(), 3U);
    CY_CHECK_EQ(held->texel(4, 4).used(), 2U);
    CY_CHECK_EQ(held->biome[(4 * kTileTexels) + 4], 7U);
    CY_CHECK(held->hole(10, 10));
    CY_CHECK_EQ(store.diagnostics().hole_quads, 1U);

    const cy::u64 bytes = store.bytes_resident();
    CY_REQUIRE(bytes > 0);
    CY_REQUIRE(store.evict(TileCoord{0, 0, 0}).has_value());
    CY_CHECK_EQ(store.bytes_resident(), 0U);
    CY_CHECK(store.find(TileCoord{0, 0, 0}) == nullptr);
}

CY_TEST_CASE("terrain tiles are a cell channel, and a cell's footprint is arithmetic") {
    const TileLayout shape = test::layout();
    const cy::world::PartitionConfig config = test::partition();

    // `world::Channel::Terrain` is the channel the specification asks for, and a dedicated server
    // requires it: it resolves collision against the ground and rebuilds navigation over it.
    CY_CHECK(cy::world::profile_channels(cy::world::WorldProfile::DedicatedServer)
                 .has(cy::world::Channel::Terrain));
    CY_CHECK(std::string_view(cy::world::channel_name(cy::world::Channel::Terrain)) == "terrain");

    // A 128 m cell over 256 m tiles: one tile, or two where the cell straddles a boundary.
    cy::Array<TileCoord> footprint(test::allocator());
    CY_REQUIRE(cell_tile_footprint(config, cy::world::CellCoord{0, 0, 0}, shape, 0, footprint)
                   .has_value());
    CY_REQUIRE_EQ(footprint.size(), 1U);
    CY_CHECK_EQ(footprint[0], (TileCoord{0, 0, 0}));

    footprint.clear();
    CY_REQUIRE(cell_tile_footprint(config, cy::world::CellCoord{1, 0, 0}, shape, 0, footprint)
                   .has_value());
    // The cell spans x in [128, 256], whose closing edge is the next tile's first sample line.
    CY_CHECK_EQ(footprint.size(), 2U);

    // And a coarse level covers the same cell with one tile, which is what makes a macro payload
    // cheap for a cell that is not near the viewer.
    footprint.clear();
    CY_REQUIRE(cell_tile_footprint(config, cy::world::CellCoord{1, 0, 0}, shape, 3, footprint)
                   .has_value());
    CY_CHECK_EQ(footprint.size(), 1U);
}
