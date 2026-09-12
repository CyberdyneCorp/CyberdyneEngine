// The modifier stack: determinism, a sculpt that survives an insertion beneath it, the derivation
// key, the scoped re-evaluation of one edit, the halo that keeps a tile independent of its
// neighbours, and the cook that flattens the stack away. M10 task 2.2.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL, AND WHAT THAT EXPOSED IN THE SUITE ITSELF.
//
// `ModifierStack::halo_samples()` was changed to return zero. The halo case AS FIRST WRITTEN —
// comparing two adjacent tiles' shared boundary column — STILL PASSED, because an erosion pass
// never writes a tile's outermost sample and the two neighbours therefore agree there whether the
// halo is six samples or none. That is the trap this project has paid for before: a test whose
// scene cannot contend.
//
// The case was rewritten to compare the same piece of world under two layouts whose origins differ
// by half a tile — a sample one step inside one layout's tile edge is thirty-two steps inside the
// other's — and the same mutation then went red on the first sample, by tens of centimetres. The
// halo was restored and the case went green. Both halves are reported in this milestone's
// `verified_failing`.

#include <cy/test/test.h>

#include "fixtures.h"

namespace test = cy::terrain::test;
using namespace cy::terrain;

namespace {

[[nodiscard]] Modifier noise_modifier() noexcept {
    Modifier modifier;
    modifier.kind = ModifierKind::Noise;
    modifier.name = "ridges";
    modifier.bounds = TerrainBounds{-10000.0, -10000.0, 10000.0, 10000.0};
    modifier.amplitude = 12.0F;
    modifier.period = 64.0F;
    modifier.iterations = 3;
    return modifier;
}

[[nodiscard]] TerrainGenerator generator() noexcept {
    TerrainGenerator value;
    value.period = 512.0F;
    value.amplitude = 90.0F;
    value.octaves = 4;
    value.base_height = 120.0F;
    return value;
}

}  // namespace

CY_TEST_CASE("evaluation is deterministic: the same stack and tile produce the same bytes") {
    const TileLayout shape = test::layout();
    ModifierStack stack(test::allocator(), shape, 0xC0FFEE);
    stack.set_generator(generator());
    CY_REQUIRE(stack.add(noise_modifier()).has_value());

    cy::Expected<TerrainTile, cy::Error> first =
        stack.evaluate(test::allocator(), TileCoord{2, -3, 0});
    cy::Expected<TerrainTile, cy::Error> second =
        stack.evaluate(test::allocator(), TileCoord{2, -3, 0});
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    for (cy::usize index = 0; index < kTileHeights; ++index) {
        CY_REQUIRE_EQ(first.value().heights[index], second.value().heights[index]);
    }
    CY_CHECK_EQ(first.value().derivation_key, second.value().derivation_key);
    CY_CHECK_NE(first.value().derivation_key, stack.derivation_key(TileCoord{3, -3, 0}));
}

CY_TEST_CASE("inserting erosion beneath a sculpt preserves the sculpt and reapplies it above") {
    const TileLayout shape = test::layout();
    ModifierStack stack(test::allocator(), shape, 7);
    stack.set_generator(generator());

    Modifier sculpt;
    sculpt.kind = ModifierKind::Sculpt;
    sculpt.name = "the artist's hill";
    sculpt.bounds = TerrainBounds{64.0, 64.0, 128.0, 128.0};
    sculpt.amplitude = 1.0F;
    cy::Expected<cy::u32, cy::Error> sculpt_index = stack.add(sculpt);
    CY_REQUIRE(sculpt_index.has_value());

    cy::f32 offsets[25];
    for (cy::f32& offset : offsets) {
        offset = 30.0F;
    }
    CY_REQUIRE(stack.add_sculpt(sculpt_index.value(), 5, cy::Span<const cy::f32>(offsets, 25))
                   .has_value());

    // The sculpt's contribution is measured AGAINST THE SAME TILE WITH THE SCULPT DISABLED, not
    // against another position: the generator is noise, so two positions differ for reasons that
    // have nothing to do with the sculpt.
    CY_REQUIRE(stack.set_enabled(sculpt_index.value(), false).has_value());
    cy::Expected<TerrainTile, cy::Error> without =
        stack.evaluate(test::allocator(), TileCoord{0, 0, 0});
    CY_REQUIRE(stack.set_enabled(sculpt_index.value(), true).has_value());
    cy::Expected<TerrainTile, cy::Error> with =
        stack.evaluate(test::allocator(), TileCoord{0, 0, 0});
    CY_REQUIRE(without.has_value());
    CY_REQUIRE(with.has_value());
    // Sample (24, 24) is at x = z = 96 m, inside the sculpt, and the stamp is thirty metres.
    CY_CHECK_NEAR(
        tile_height(shape, with.value(), 24, 24) - tile_height(shape, without.value(), 24, 24),
        30.0F, 0.01F);

    // The author inserts an erosion pass BENEATH the sculpt, which is the scenario's own operation.
    Modifier erosion;
    erosion.kind = ModifierKind::Erosion;
    erosion.name = "thermal";
    erosion.bounds = TerrainBounds{-10000.0, -10000.0, 10000.0, 10000.0};
    erosion.amplitude = 0.5F;
    erosion.iterations = 4;
    CY_REQUIRE(stack.insert_at(0, erosion).has_value());
    CY_REQUIRE_EQ(stack.modifiers()[0].kind, ModifierKind::Erosion);
    CY_REQUIRE_EQ(stack.modifiers()[1].kind, ModifierKind::Sculpt);

    CY_REQUIRE(stack.set_enabled(1, false).has_value());
    cy::Expected<TerrainTile, cy::Error> eroded_without =
        stack.evaluate(test::allocator(), TileCoord{0, 0, 0});
    CY_REQUIRE(stack.set_enabled(1, true).has_value());
    cy::Expected<TerrainTile, cy::Error> eroded_with =
        stack.evaluate(test::allocator(), TileCoord{0, 0, 0});
    CY_REQUIRE(eroded_without.has_value());
    CY_REQUIRE(eroded_with.has_value());

    // Erosion changed the ground — the terrain beneath the sculpt is not what it was — and the
    // sculpt is still exactly thirty metres above whatever is now underneath it. A sculpt baked
    // into the heightfield would have been smoothed away with the ground.
    CY_CHECK_NE(tile_height(shape, eroded_without.value(), 5, 5),
                tile_height(shape, without.value(), 5, 5));
    CY_CHECK_NEAR(tile_height(shape, eroded_with.value(), 24, 24) -
                      tile_height(shape, eroded_without.value(), 24, 24),
                  30.0F, 0.01F);
}

CY_TEST_CASE("reordering, disabling and editing all change the derivation key") {
    const TileLayout shape = test::layout();
    ModifierStack stack(test::allocator(), shape, 11);
    stack.set_generator(generator());

    Modifier flatten;
    flatten.kind = ModifierKind::Flatten;
    flatten.name = "build site";
    flatten.bounds = TerrainBounds{0.0, 0.0, 64.0, 64.0};
    flatten.height = 100.0F;
    CY_REQUIRE(stack.add(noise_modifier()).has_value());
    CY_REQUIRE(stack.add(flatten).has_value());

    const TileCoord coord{0, 0, 0};
    const cy::u64 original = stack.derivation_key(coord);

    CY_REQUIRE(stack.move(0, 1).has_value());
    const cy::u64 reordered = stack.derivation_key(coord);
    CY_CHECK_NE(original, reordered);
    CY_REQUIRE(stack.move(1, 0).has_value());
    CY_CHECK_EQ(stack.derivation_key(coord), original);

    CY_REQUIRE(stack.set_enabled(1, false).has_value());
    CY_CHECK_NE(stack.derivation_key(coord), original);
    CY_REQUIRE(stack.set_enabled(1, true).has_value());
    CY_CHECK_EQ(stack.derivation_key(coord), original);

    // A disabled modifier contributes nothing, which is what "disabled" has to mean for a
    // non-destructive stack.
    CY_REQUIRE(stack.set_enabled(1, false).has_value());
    cy::Expected<TerrainTile, cy::Error> without = stack.evaluate(test::allocator(), coord);
    CY_REQUIRE(stack.set_enabled(1, true).has_value());
    cy::Expected<TerrainTile, cy::Error> with = stack.evaluate(test::allocator(), coord);
    CY_REQUIRE(without.has_value());
    CY_REQUIRE(with.has_value());
    CY_CHECK_NE(with.value().stored(4, 4), without.value().stored(4, 4));
}

CY_TEST_CASE("one road edit is scoped to the tiles its stamp and declared radius reach") {
    const TileLayout shape = test::layout();
    ModifierStack stack(test::allocator(), shape, 3);
    stack.set_generator(generator());

    Modifier road;
    road.kind = ModifierKind::RoadSpline;
    road.name = "the valley road";
    road.bounds = TerrainBounds{100.0, 100.0, 400.0, 140.0};
    road.radius = 20.0F;
    road.period = 12.0F;
    road.height = 110.0F;
    road.layer = 9;
    road.suppresses_foliage = true;
    cy::Expected<cy::u32, cy::Error> index = stack.add(road);
    CY_REQUIRE(index.has_value());
    const TerrainPoint points[3] = {{110.0, 120.0}, {250.0, 120.0}, {390.0, 120.0}};
    CY_REQUIRE(
        stack.add_spline(index.value(), cy::Span<const TerrainPoint>(points, 3)).has_value());
    CY_REQUIRE(stack.declare_decoration(9, true).has_value());

    cy::Array<TileCoord> dirty(test::allocator());
    CY_REQUIRE(stack.dirty_tiles(index.value(), 0, dirty).has_value());
    // Bounds plus twenty metres, over 256 m tiles: x from 80 to 420 and z from 80 to 160, which is
    // tiles 0 and 1 in x and tile 0 in z.
    CY_REQUIRE_EQ(dirty.size(), 2U);
    for (const TileCoord& coord : dirty.span()) {
        CY_CHECK(tile_bounds(shape, coord).overlaps(modifier_reach(stack.modifiers()[0])));
    }

    // A tile the road does not reach is untouched by moving it, which is what "only the regions its
    // stamp and declared radius reach SHALL be re-evaluated" buys.
    const TileCoord far{5, 5, 0};
    cy::Expected<TerrainTile, cy::Error> before = stack.evaluate(test::allocator(), far);
    CY_REQUIRE(before.has_value());
    const TerrainPoint moved[3] = {{110.0, 130.0}, {250.0, 130.0}, {390.0, 130.0}};
    CY_REQUIRE(stack.add_spline(index.value(), cy::Span<const TerrainPoint>(moved, 3)).has_value());
    cy::Expected<TerrainTile, cy::Error> after = stack.evaluate(test::allocator(), far);
    CY_REQUIRE(after.has_value());
    for (cy::usize sample = 0; sample < kTileHeights; ++sample) {
        CY_REQUIRE_EQ(before.value().heights[sample], after.value().heights[sample]);
    }
}

CY_TEST_CASE("a road blends into the material and suppresses foliage along its width") {
    const TileLayout shape = test::layout();
    ModifierStack stack(test::allocator(), shape, 3);
    stack.set_generator(generator());

    Modifier road;
    road.kind = ModifierKind::RoadSpline;
    road.name = "the forest road";
    road.bounds = TerrainBounds{0.0, 100.0, 256.0, 140.0};
    road.period = 16.0F;
    road.height = 120.0F;
    road.layer = 9;
    cy::Expected<cy::u32, cy::Error> index = stack.add(road);
    CY_REQUIRE(index.has_value());
    const TerrainPoint points[2] = {{0.0, 120.0}, {256.0, 120.0}};
    CY_REQUIRE(
        stack.add_spline(index.value(), cy::Span<const TerrainPoint>(points, 2)).has_value());
    CY_REQUIRE(stack.declare_decoration(9, true).has_value());

    cy::Expected<TerrainTile, cy::Error> tile =
        stack.evaluate(test::allocator(), TileCoord{0, 0, 0});
    CY_REQUIRE(tile.has_value());

    // Texel 30 is z in [120, 124), on the road; texel 5 is z in [20, 24), off it.
    CY_CHECK_EQ(tile.value().texel(10, 30).dominant(), 9U);
    CY_CHECK_NE(tile.value().texel(10, 5).dominant(), 9U);
    CY_CHECK(stack.suppresses_foliage(9));
    CY_CHECK_FALSE(stack.suppresses_foliage(0));
    // And the surface was levelled toward the spline's height along its width.
    CY_CHECK_NEAR(tile_height(shape, tile.value(), 10, 30), 120.0F, 2.0F);
}

CY_TEST_CASE("an erosion pass reads its declared halo, so the tiling does not show") {
    // THE TEST THAT ACTUALLY CONTENDS. Comparing two adjacent tiles' shared column proves nothing
    // about a halo: an erosion pass never writes a tile's outermost sample, so two neighbours agree
    // there whether the halo is six samples or none. What a missing halo changes is the samples
    // JUST INSIDE the boundary, and the only thing to compare them against is the same piece of
    // world evaluated where it is NOT near a boundary.
    //
    // So the same terrain is evaluated under two layouts whose origins differ by half a tile. The
    // generator is a function of world position, so a correct evaluation is too; a sample one step
    // inside one layout's tile edge is thirty-two steps inside the other's, and the two must agree.
    const TileLayout shape = test::layout();
    TileLayout shifted = shape;
    shifted.origin_x = -128.0;

    ModifierStack stack(test::allocator(), shape, 42);
    ModifierStack offset(test::allocator(), shifted, 42);
    Modifier erosion;
    erosion.kind = ModifierKind::Erosion;
    erosion.name = "thermal";
    erosion.bounds = TerrainBounds{-10000.0, -10000.0, 10000.0, 10000.0};
    erosion.amplitude = 0.6F;
    erosion.iterations = 6;
    stack.set_generator(generator());
    offset.set_generator(generator());
    CY_REQUIRE(stack.add(erosion).has_value());
    CY_REQUIRE(offset.add(erosion).has_value());

    cy::Expected<TerrainTile, cy::Error> aligned =
        stack.evaluate(test::allocator(), TileCoord{1, 0, 0});
    cy::Expected<TerrainTile, cy::Error> interior =
        offset.evaluate(test::allocator(), TileCoord{1, 0, 0});
    CY_REQUIRE(aligned.has_value());
    CY_REQUIRE(interior.has_value());

    // Sample i of the aligned tile is at x = (64 + i) * 4; the shifted layout puts that same point
    // at -128 + (64 + k) * 4, so k = i + 32. The first eight samples inside the edge are the ones a
    // zero halo gets wrong, and they are the ones compared.
    for (cy::u32 i = 0; i < 8; ++i) {
        for (cy::u32 j = 0; j < kTileVerts; ++j) {
            CY_REQUIRE_EQ(aligned.value().stored(i, j), interior.value().stored(i + 32, j));
        }
    }

    // And two adjacent tiles still share their boundary column exactly, which is the property the
    // meshing suite then rests on.
    cy::Expected<TerrainTile, cy::Error> left =
        stack.evaluate(test::allocator(), TileCoord{0, 0, 0});
    CY_REQUIRE(left.has_value());
    for (cy::u32 j = 0; j < kTileVerts; ++j) {
        CY_REQUIRE_EQ(left.value().stored(kTileQuads, j), aligned.value().stored(0, j));
    }
}

CY_TEST_CASE("a hole modifier cuts the surface, and cooking flattens the stack away") {
    const TileLayout shape = test::layout();
    ModifierStack stack(test::allocator(), shape, 5);
    stack.set_generator(generator());

    Modifier hole;
    hole.kind = ModifierKind::Hole;
    hole.name = "the cave mouth";
    hole.bounds = TerrainBounds{40.0, 40.0, 60.0, 60.0};
    CY_REQUIRE(stack.add(hole).has_value());

    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(stack.flatten(store, 0, 0, 1, 1, 0).has_value());
    CY_CHECK_EQ(store.tile_count(), 4U);

    const TerrainTile* tile = store.find(TileCoord{0, 0, 0});
    CY_REQUIRE(tile != nullptr);
    // Texel 12 spans [48, 52) in both axes, inside the hole; texel 0 is not.
    CY_CHECK(tile->hole(12, 12));
    CY_CHECK_FALSE(tile->hole(0, 0));

    // The cooked tile carries the derivation key it was produced under — a cook cache and an
    // incremental re-cook key on it — and it carries no stack: `TerrainStore` has no way to name
    // one, which is "the runtime SHALL NOT carry it" made structural rather than asserted.
    CY_CHECK_EQ(tile->derivation_key, stack.derivation_key(TileCoord{0, 0, 0}));
    CY_CHECK_NE(tile->derivation_key, 0U);
}
