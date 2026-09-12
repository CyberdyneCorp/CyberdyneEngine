// The query interface: a column rather than a height, the representations that answer it, holes,
// and the resolution indicator an unstreamed region comes back with. M10 task 2.1, and `terrain`'s
// "Terrain representations" and "Terrain queries".
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: the hole branch in `HeightfieldSource::column()` was
// deleted, and "a hole is not a surface, and every consumer agrees" went red on
// `CY_REQUIRE_FALSE(sample.resolved)` with the query reporting ground across a cave mouth. It was
// then restored. The sequence is reported in this milestone's `verified_failing`.

#include <cy/test/test.h>

#include "fixtures.h"

namespace test = cy::terrain::test;
using namespace cy::terrain;

CY_TEST_CASE("a query answers height, normal, slope and material without a physics query") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    TerrainTile tile = test::make_tile(shape, TileCoord{0, 0, 0}, test::ramp);
    tile.texel(2, 2) = MaterialTexel{{5, 0, 0, 0}, {255, 0, 0, 0}};
    tile.biome[(2 * kTileTexels) + 2] = 3;
    CY_REQUIRE(store.insert(std::move(tile)).has_value());

    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);
    TerrainQuery query(test::allocator());
    CY_REQUIRE(query.add_source(heights).has_value());

    // The ramp rises one metre every eight, so the surface at x = 64 is at 8 m and its slope is
    // atan(0.125) = 7.125 degrees. Both are arithmetic the test does for itself.
    const SurfaceSample sample = query.sample(64.0, 40.0);
    CY_REQUIRE(sample.resolved);
    CY_CHECK_NEAR(sample.height, 8.0F, 0.05F);
    CY_CHECK_NEAR(sample.slope_degrees, 7.125F, 0.2F);
    CY_CHECK_GT(sample.normal.y, 0.9F);
    CY_CHECK_LT(sample.normal.x, 0.0F);

    const SurfaceSample material = query.sample(10.0, 10.0);
    CY_CHECK_EQ(material.layer, 5U);
    CY_CHECK_EQ(material.biome, 3U);
}

CY_TEST_CASE("a column can carry more than one surface, and no consumer assumes a heightmap") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 0}, test::flat)).has_value());
    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);

    // An arch: two triangles of the MESH representation, thirty metres above the ground.
    MeshSource arch(test::allocator());
    const WorldTriangle first{TerrainPoint{40.0, 40.0},
                              TerrainPoint{80.0, 40.0},
                              TerrainPoint{40.0, 80.0},
                              130.0F,
                              130.0F,
                              130.0F,
                              9,
                              1};
    const WorldTriangle second{TerrainPoint{80.0, 40.0},
                               TerrainPoint{80.0, 80.0},
                               TerrainPoint{40.0, 80.0},
                               130.0F,
                               130.0F,
                               130.0F,
                               9,
                               1};
    CY_REQUIRE(arch.add_triangle(first).has_value());
    CY_REQUIRE(arch.add_triangle(second).has_value());

    TerrainQuery query(test::allocator());
    CY_REQUIRE(query.add_source(heights).has_value());
    CY_REQUIRE(query.add_source(arch).has_value());
    CY_CHECK_EQ(query.source_count(), 2U);

    SurfaceSample column[kMaxColumnSurfaces];
    // Deliberately off the triangles' shared diagonal (x + z = 120): a point exactly on a shared
    // edge belongs to both triangles, and a column of three would be a fact about the fixture
    // rather than about the arch.
    const cy::u32 count = query.column(55.0, 60.0, cy::Span<SurfaceSample>(column, 4));
    CY_REQUIRE_EQ(count, 2U);
    // Highest first, and the two came from different representations.
    CY_CHECK_NEAR(column[0].height, 130.0F, 0.01F);
    CY_CHECK_EQ(column[0].from, Representation::Mesh);
    CY_CHECK_NEAR(column[1].height, 100.0F, 0.01F);
    CY_CHECK_EQ(column[1].from, Representation::Heightfield);

    // A character standing under the arch wants the ground, not the arch. That is what `below()`
    // is for, and a `height_at()` returning one float could not have answered it.
    CY_CHECK_NEAR(query.below(55.0, 60.0, 110.0F).height, 100.0F, 0.01F);
    CY_CHECK_NEAR(query.above(55.0, 60.0, 110.0F).height, 130.0F, 0.01F);
}

CY_TEST_CASE("a hole is not a surface, and every consumer agrees") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    TerrainTile tile = test::make_tile(shape, TileCoord{0, 0, 0}, test::flat);
    tile.set_hole(5, 5, true);
    CY_REQUIRE(store.insert(std::move(tile)).has_value());
    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);
    TerrainQuery query(test::allocator());
    CY_REQUIRE(query.add_source(heights).has_value());

    // Quad (5, 5) of a 4 m lattice spans x in [20, 24) and z in [20, 24).
    const SurfaceSample sample = query.sample(22.0, 22.0);
    CY_REQUIRE_FALSE(sample.resolved);
    CY_CHECK(sample.hole);
    // And the surface beside it is ordinary ground, so the hole is a hole and not an outage.
    CY_CHECK(query.sample(50.0, 50.0).resolved);
}

CY_TEST_CASE(
    "a query into unstreamed terrain answers from the coarsest resident level and says so") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    // Only level 2 is resident: 16 m per sample rather than 4 m.
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 2}, test::ramp)).has_value());
    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);
    TerrainQuery query(test::allocator());
    CY_REQUIRE(query.add_source(heights).has_value());

    const SurfaceSample sample = query.sample(300.0, 300.0);
    CY_REQUIRE(sample.resolved);
    CY_CHECK_EQ(sample.level, 2U);
    CY_CHECK_NEAR(sample.sample_metres, 16.0F, 0.001F);
    // The answer is still the right one — a ramp is a ramp at any resolution — but the consumer is
    // TOLD which resolution it is, which is the requirement.
    CY_CHECK_NEAR(sample.height, 37.5F, 0.5F);

    // And a position with nothing resident anywhere does not fault and does not block.
    const SurfaceSample missing = query.sample(90000.0, 90000.0);
    CY_CHECK_FALSE(missing.resolved);
}

CY_TEST_CASE("queries are batchable, and a batch answers what one at a time answers") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 0}, test::ramp)).has_value());
    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);
    TerrainQuery query(test::allocator());
    CY_REQUIRE(query.add_source(heights).has_value());

    cy::Array<TerrainPoint> positions(test::allocator());
    for (cy::u32 index = 0; index < 512; ++index) {
        CY_REQUIRE(positions
                       .push_back(TerrainPoint{static_cast<cy::f64>(index) * 0.4,
                                               static_cast<cy::f64>(index) * 0.3})
                       .has_value());
    }
    cy::Array<SurfaceSample> batched(test::allocator());
    CY_REQUIRE(batched.resize(positions.size()).has_value());
    CY_REQUIRE(query.sample_many(positions.span(), batched.span()).has_value());

    for (cy::usize index = 0; index < positions.size(); ++index) {
        const SurfaceSample one = query.sample(positions[index].x, positions[index].z);
        CY_REQUIRE_EQ(batched[index].resolved, one.resolved);
        CY_REQUIRE_EQ(batched[index].height, one.height);
        CY_REQUIRE_EQ(batched[index].level, one.level);
    }

    // A buffer that cannot hold the batch is refused rather than truncated.
    cy::Array<SurfaceSample> small(test::allocator());
    CY_REQUIRE(small.resize(4).has_value());
    CY_CHECK_FALSE(query.sample_many(positions.span(), small.span()).has_value());
}

CY_TEST_CASE("a ray against the surface finds the crossing, and misses when there is none") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 0}, test::flat)).has_value());
    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);
    TerrainQuery query(test::allocator());
    CY_REQUIRE(query.add_source(heights).has_value());

    const RayHit down = query.raycast(100.0, 150.0F, 100.0, cy::Vec3{0.0F, -1.0F, 0.0F}, 100.0F);
    CY_REQUIRE(down.hit);
    CY_CHECK_NEAR(down.distance, 50.0F, 0.5F);
    CY_CHECK_NEAR(down.surface.height, 100.0F, 0.05F);

    const RayHit up = query.raycast(100.0, 150.0F, 100.0, cy::Vec3{0.0F, 1.0F, 0.0F}, 100.0F);
    CY_CHECK_FALSE(up.hit);
}
