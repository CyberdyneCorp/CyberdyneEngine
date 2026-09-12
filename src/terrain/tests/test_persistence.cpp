// The delta's two journeys out of memory: into the world's persistence overlay and back, and into a
// bounded replication message and back. M10 task 2.2, and `terrain`'s "Terrain deltas use the
// persistence overlay".
//
// An integration suite because both cases deform a world several times over — a crater is a stamp
// over every sample of every tile it touches — and the taxonomy in `testing-and-quality` places a
// case that expensive in the next suite up. Making them cheaper would mean deforming a world too
// small for "the message is proportional to the region rather than to the world" to mean anything.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: `TerrainDeltaStore::encode_region()` had its
// `overlaps()` test removed so that every tile was encoded, and "a networked terrain change is a
// bounded message" went red on `CHECK_LT(local.size() * 4, whole.size())` with the two messages the
// same size. It was then restored. The sequence is reported in this milestone's `verified_failing`.

#include <cy/test/test.h>

#include <cy/world/overlay.h>

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
    deformation.cause = "test excavation";
    return deformation;
}

}  // namespace

CY_TEST_CASE("a mining pit survives a save, through the world's own persistence overlay") {
    const TileLayout shape = test::layout();
    const cy::world::PartitionConfig config = test::partition();
    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 0}, test::flat)).has_value());

    TerrainDeltaStore deltas(test::allocator(), shape);
    CY_REQUIRE(deltas.apply(crater(60.0, 60.0, 10.0, -9.0F)).has_value());

    HeightfieldSource heights(store, &deltas);
    TerrainQuery query(test::allocator());
    CY_REQUIRE(query.add_source(heights).has_value());
    const cy::f32 dug = query.sample(60.0, 60.0).height;
    CY_REQUIRE(dug < 95.0F);

    cy::world::PersistenceOverlay overlay(test::allocator());
    CY_REQUIRE(deltas.record_into(overlay, config).has_value());
    CY_CHECK_GT(overlay.cell_count(), 0U);

    // The session ends. A fresh store, the same cooked terrain, and the overlay.
    TerrainDeltaStore reloaded(test::allocator(), shape);
    CY_REQUIRE(reloaded.restore_from(overlay, config).has_value());
    HeightfieldSource restored_heights(store, &reloaded);
    TerrainQuery restored(test::allocator());
    CY_REQUIRE(restored.add_source(restored_heights).has_value());
    CY_CHECK_EQ(restored.sample(60.0, 60.0).height, dug);
    CY_CHECK_EQ(restored.sample(200.0, 30.0).height, query.sample(200.0, 30.0).height);
}

CY_TEST_CASE("a networked terrain change is a bounded message, not a resend of terrain") {
    const TileLayout shape = test::layout();
    TerrainDeltaStore deltas(test::allocator(), shape);

    // A world of deltas: six craters, spread over a kilometre and a half.
    for (cy::i32 index = 0; index < 6; ++index) {
        const cy::f64 at = 200.0 + (static_cast<cy::f64>(index) * 256.0);
        CY_REQUIRE(deltas.apply(crater(at, at, 8.0, -4.0F)).has_value());
    }
    // Six craters, six level-0 tiles carrying a delta — and no coarse copies of any of them, which
    // is what keeps the message below proportional to the region rather than to the hierarchy.
    CY_REQUIRE_EQ(deltas.deformed_tiles(), 6U);

    cy::Array<cy::u8> whole(test::allocator());
    CY_REQUIRE(
        deltas.encode_region(TerrainBounds{-1.0e6, -1.0e6, 1.0e6, 1.0e6}, whole).has_value());

    cy::Array<cy::u8> local(test::allocator());
    CY_REQUIRE(deltas.encode_region(TerrainBounds{190.0, 190.0, 210.0, 210.0}, local).has_value());

    // The message is proportional to the region it names, not to the world's deformation: one
    // crater of six, so a sixth of the bytes give or take a header.
    CY_CHECK_GT(local.size(), 0U);
    CY_CHECK_LT(local.size() * 4, whole.size());

    // And applying it reproduces exactly those heights on the receiving side.
    TerrainDeltaStore peer(test::allocator(), shape);
    cy::Expected<cy::u32, cy::Error> applied = peer.apply_encoded(local.span());
    CY_REQUIRE(applied.has_value());
    CY_CHECK_GT(applied.value(), 0U);
    const TileCoord centre = tile_at(shape, 200.0, 200.0, 0);
    for (cy::u32 j = 0; j < kTileVerts; ++j) {
        for (cy::u32 i = 0; i < kTileVerts; ++i) {
            CY_REQUIRE_EQ(peer.height_delta(centre, i, j), deltas.height_delta(centre, i, j));
        }
    }
    // What the message did NOT carry stays absent: a far crater is not in it.
    CY_CHECK_FALSE(peer.has_delta(tile_at(shape, 4000.0, 4000.0, 0)));
}
