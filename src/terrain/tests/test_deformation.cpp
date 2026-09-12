// Deformation classes, the delta over immutable cooked terrain, the overlay round trip, and the
// bounded replication message. M10 task 2.2, and `terrain`'s "Deformation classes" and "Terrain
// deltas use the persistence overlay".
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: `TerrainDeltaStore::stamp_tile()` was changed to
// write the GAMEPLAY channel for a `Visual` deformation, and "a footprint is cheap" went red on
// `CY_CHECK_NEAR(after.height, before.height, 0.0001F)` with a footprint having moved the ground a
// gameplay query stands on. It was then restored. The sequence is reported in this milestone's
// `verified_failing`.

#include <cy/test/test.h>

#include "fixtures.h"

namespace test = cy::terrain::test;
using namespace cy::terrain;

namespace {

[[nodiscard]] Deformation crater(cy::f64 x, cy::f64 z, cy::f64 radius, cy::f32 depth) noexcept {
    Deformation deformation;
    deformation.klass = DeformationClass::Gameplay;
    deformation.shape = DeformationShape::Radial;
    deformation.bounds = TerrainBounds{x - radius, z - radius, x + radius, z + radius};
    deformation.amount = depth;
    deformation.cause = "test explosion";
    return deformation;
}

[[nodiscard]] cy::u64 height_digest(const TerrainTile& tile) noexcept {
    cy::u64 digest = 1469598103934665603ULL;
    for (const cy::u16 sample : tile.heights.span()) {
        digest = cy::hash_combine(digest, sample);
    }
    return digest;
}

}  // namespace

CY_TEST_CASE("a footprint is cheap: visual deformation invalidates rendering and nothing else") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 0}, test::flat)).has_value());
    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);
    TerrainQuery query(test::allocator());
    CY_REQUIRE(query.add_source(heights).has_value());

    const SurfaceSample before = query.sample(100.0, 100.0);

    Deformation footprint = crater(100.0, 100.0, 0.5, -0.05F);
    footprint.klass = DeformationClass::Visual;
    cy::Expected<InvalidationSet, cy::Error> applied = deltas.apply(footprint);
    CY_REQUIRE(applied.has_value());

    CY_CHECK(applied.value().rendering);
    CY_CHECK_FALSE(applied.value().collision);
    CY_CHECK_FALSE(applied.value().navigation);
    CY_CHECK(applied.value().navigation_dirty.empty());

    // And the gameplay surface did not move. This is the half a flag beside one array cannot give.
    const SurfaceSample after = query.sample(100.0, 100.0);
    CY_CHECK_NEAR(after.height, before.height, 0.0001F);
    // The rendering displacement IS there: the two channels are separate, not absent.
    CY_CHECK_LT(deltas.visual_delta(TileCoord{0, 0, 0}, 25, 25), 0.0F);
}

CY_TEST_CASE("a crater is not: it moves the surface and invalidates collision and navigation") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 0}, test::flat)).has_value());
    const cy::u64 cooked = height_digest(*store.find(TileCoord{0, 0, 0}));

    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);
    TerrainQuery query(test::allocator());
    CY_REQUIRE(query.add_source(heights).has_value());

    cy::Expected<InvalidationSet, cy::Error> applied =
        deltas.apply(crater(100.0, 100.0, 12.0, -6.0F));
    CY_REQUIRE(applied.has_value());
    CY_CHECK(applied.value().rendering);
    CY_CHECK(applied.value().collision);
    CY_CHECK(applied.value().navigation);
    CY_REQUIRE_EQ(applied.value().navigation_dirty.size(), 1U);

    CY_CHECK_NEAR(query.sample(100.0, 100.0).height, 94.0F, 0.5F);
    CY_CHECK_NEAR(query.sample(200.0, 200.0).height, 100.0F, 0.01F);

    // `cooked terrain + terrain delta = current terrain`: the cooked bytes are untouched.
    CY_CHECK_EQ(height_digest(*store.find(TileCoord{0, 0, 0})), cooked);

    // Only the affected region, at tile granularity. Every named tile actually overlaps the edit.
    for (const TileCoord& coord : applied.value().tiles.span()) {
        CY_CHECK(tile_bounds(shape, coord).overlaps(applied.value().navigation_dirty[0]));
    }
}

CY_TEST_CASE("a structural deformation is refused, naming the representation it needs") {
    const TileLayout shape = test::layout();
    TerrainDeltaStore deltas(test::allocator(), shape);
    Deformation tunnel = crater(100.0, 100.0, 4.0, -3.0F);
    tunnel.klass = DeformationClass::Structural;

    cy::Expected<InvalidationSet, cy::Error> applied = deltas.apply(tunnel);
    CY_REQUIRE_FALSE(applied.has_value());
    CY_CHECK_EQ(applied.error().code, cy::ErrorCode::NotImplemented);
    CY_CHECK_EQ(deltas.deformed_tiles(), 0U);

    // And the same refusal for opening a hole at run time, which is the same edit by another name.
    Deformation hole = crater(100.0, 100.0, 4.0, 0.0F);
    hole.opens_hole = true;
    CY_CHECK_FALSE(deltas.apply(hole).has_value());
}

CY_TEST_CASE("a deformation reaches every level, so a coarse query sees the same world") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 2}, test::flat)).has_value());
    TerrainDeltaStore deltas(test::allocator(), shape);
    CY_REQUIRE(deltas.apply(crater(300.0, 300.0, 40.0, -10.0F)).has_value());

    // Only the coarse tile is resident, and it carries the crater: a query that fell back to a
    // macro level would otherwise report undisturbed ground where a pit is.
    HeightfieldSource heights(store, &deltas);
    TerrainQuery query(test::allocator());
    CY_REQUIRE(query.add_source(heights).has_value());
    CY_CHECK_LT(query.sample(300.0, 300.0).height, 96.0F);
    CY_CHECK_EQ(query.sample(300.0, 300.0).level, 2U);
}
