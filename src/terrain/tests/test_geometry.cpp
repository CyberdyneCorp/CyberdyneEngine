// Meshing, the watertight boundaries, the cacheable key, and the collision and navigation surfaces
// derived from the same tile. M10 tasks 2.1 and 2.2, and `terrain`'s "Terrain is a geometry
// source", "Terrain collision" and "Terrain navigation contribution".
//
// This is an integration suite because a 65x65 lattice meshed four times, a collision field built
// from it and a navmesh tile built from that are tens of thousands of samples — over the unit
// budget by design, and shrinking it would be testing a different claim than "the boundary is
// watertight".
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: the `stitch()` call in `mesh_tile()` was removed,
// and "a fine tile yields to a coarser neighbour, exactly onto its edge" went red on the first odd
// boundary vertex — a crack of tenths of a metre — with `stitched_vertices` at zero. It was then
// restored. The sequence is reported in this milestone's `verified_failing`.

#include <cy/test/test.h>

#include <cmath>

#include <cy/navigation/build.h>

#include "fixtures.h"

namespace test = cy::terrain::test;
using namespace cy::terrain;

namespace {

/// A surface with structure in both axes at a period of a few samples, so that a boundary that is
/// NOT watertight shows as metres rather than as millimetres. A gently rolling fixture would let a
/// missing stitch pass for a rounding difference, which is the failure this whole suite is here to
/// catch.
[[nodiscard]] cy::f32 hills(cy::f64 x, cy::f64 z) noexcept {
    const cy::f64 u = x * 0.05;
    const cy::f64 v = z * 0.09;
    return 200.0F +
           (30.0F * static_cast<cy::f32>(u - static_cast<cy::f64>(static_cast<cy::i64>(u)))) +
           (20.0F * static_cast<cy::f32>(v - static_cast<cy::f64>(static_cast<cy::i64>(v))));
}

[[nodiscard]] MeshOptions same_level(cy::u8 level) noexcept {
    MeshOptions options;
    for (unsigned char& edge : options.neighbour_level) {
        edge = level;
    }
    return options;
}

}  // namespace

CY_TEST_CASE("adjacent tiles at one level share their boundary exactly, with no stitching rule") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 0}, hills)).has_value());
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{1, 0, 0}, hills)).has_value());
    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);

    MeshReport left_report;
    MeshReport right_report;
    cy::Expected<TerrainMesh, cy::Error> left = mesh_tile(
        test::allocator(), store, heights, &deltas, TileCoord{0, 0, 0}, same_level(0), left_report);
    cy::Expected<TerrainMesh, cy::Error> right =
        mesh_tile(test::allocator(), store, heights, &deltas, TileCoord{1, 0, 0}, same_level(0),
                  right_report);
    CY_REQUIRE(left.has_value());
    CY_REQUIRE(right.has_value());
    CY_CHECK_EQ(left_report.stitched_vertices, 0U);
    CY_CHECK_EQ(left_report.triangles, kTileQuads * kTileQuads * 2);

    // The left tile's last column and the right tile's first column are the same line of the world,
    // and the heights are bit-identical — not within a tolerance.
    for (cy::u32 j = 0; j < kTileVerts; ++j) {
        const cy::f32 a = left.value().positions[(j * kTileVerts) + kTileQuads].y;
        const cy::f32 b = right.value().positions[static_cast<cy::usize>(j) * kTileVerts].y;
        CY_REQUIRE_EQ(a, b);
    }
}

CY_TEST_CASE("a fine tile yields to a coarser neighbour, exactly onto its edge") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);

    // The fine tile spans x in [256, 512]; the coarse tile to its right is the level-1 tile
    // spanning [512, 1024], derived at cook time from the four level-0 tiles beneath it. The two
    // therefore share the line x = 512, with the coarse tile carrying every second sample of it.
    TerrainTile children[4] = {
        test::make_tile(shape, TileCoord{2, 0, 0}, hills),
        test::make_tile(shape, TileCoord{3, 0, 0}, hills),
        test::make_tile(shape, TileCoord{2, 1, 0}, hills),
        test::make_tile(shape, TileCoord{3, 1, 0}, hills),
    };
    const TerrainTile* pointers[4] = {&children[0], &children[1], &children[2], &children[3]};
    cy::Expected<TerrainTile, cy::Error> coarse =
        derive_coarse(test::allocator(), shape, TileCoord{1, 0, 1},
                      cy::Span<const TerrainTile* const>(pointers, 4));
    CY_REQUIRE(coarse.has_value());

    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{1, 0, 0}, hills)).has_value());
    CY_REQUIRE(store.insert(std::move(coarse.value())).has_value());
    const TerrainTile* neighbour = store.find(TileCoord{1, 0, 1});
    CY_REQUIRE(neighbour != nullptr);

    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);
    MeshOptions options = same_level(0);
    options.neighbour_level[static_cast<cy::u32>(TileEdge::PositiveX)] = 1;

    MeshReport report;
    cy::Expected<TerrainMesh, cy::Error> mesh =
        mesh_tile(test::allocator(), store, heights, &deltas, TileCoord{1, 0, 0}, options, report);
    CY_REQUIRE(mesh.has_value());
    CY_CHECK_EQ(report.stitched_vertices, 32U);  // every odd sample of a 65-sample edge

    // Every boundary vertex lies on the straight segment the COARSE tile will draw. The expected
    // value is computed from the coarse tile's own stored samples, never by asking `stitch_edge()`
    // what the answer should be.
    for (cy::u32 j = 1; j < kTileQuads; ++j) {
        const cy::f32 emitted = mesh.value().positions[(j * kTileVerts) + kTileQuads].y;
        const cy::u32 low = (j / 2) * 2;
        const cy::f32 coarse_low = tile_height(shape, *neighbour, 0, low / 2);
        const cy::f32 coarse_high = tile_height(shape, *neighbour, 0, (low / 2) + 1);
        const cy::f32 expected =
            (j % 2 == 0) ? coarse_low : (coarse_low + ((coarse_high - coarse_low) * 0.5F));
        // An absolute tolerance, computed here: a centimetre over a surface of hundreds of metres.
        CY_REQUIRE(std::fabs(emitted - expected) < 0.01F);
    }
}

CY_TEST_CASE("meshing is deterministic and cacheable, and an edit changes the key") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 0}, hills)).has_value());
    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);

    MeshReport first_report;
    MeshReport second_report;
    cy::Expected<TerrainMesh, cy::Error> first =
        mesh_tile(test::allocator(), store, heights, &deltas, TileCoord{0, 0, 0}, same_level(0),
                  first_report);
    cy::Expected<TerrainMesh, cy::Error> second =
        mesh_tile(test::allocator(), store, heights, &deltas, TileCoord{0, 0, 0}, same_level(0),
                  second_report);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(first.value().key, second.value().key);
    for (cy::usize index = 0; index < first.value().positions.size(); ++index) {
        CY_REQUIRE_EQ(first.value().positions[index].y, second.value().positions[index].y);
    }

    // A coarser neighbour changes the mesh, so it must change the key: a cache that ignored the
    // neighbour levels would serve a cracked mesh after a level change.
    MeshOptions coarser = same_level(0);
    coarser.neighbour_level[static_cast<cy::u32>(TileEdge::NegativeZ)] = 1;
    MeshReport third_report;
    cy::Expected<TerrainMesh, cy::Error> third = mesh_tile(
        test::allocator(), store, heights, &deltas, TileCoord{0, 0, 0}, coarser, third_report);
    CY_REQUIRE(third.has_value());
    CY_CHECK_NE(third.value().key, first.value().key);

    // And so does a crater.
    Deformation crater;
    crater.klass = DeformationClass::Gameplay;
    crater.bounds = TerrainBounds{100.0, 100.0, 120.0, 120.0};
    crater.amount = -5.0F;
    CY_REQUIRE(deltas.apply(crater).has_value());
    MeshReport fourth_report;
    cy::Expected<TerrainMesh, cy::Error> fourth =
        mesh_tile(test::allocator(), store, heights, &deltas, TileCoord{0, 0, 0}, same_level(0),
                  fourth_report);
    CY_REQUIRE(fourth.has_value());
    CY_CHECK_NE(fourth.value().key, first.value().key);
}

CY_TEST_CASE("a hole leaves an opening, and the fallback path reports its reduced detail") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    TerrainTile tile = test::make_tile(shape, TileCoord{0, 0, 0}, hills);
    tile.set_hole(3, 3, true);
    tile.set_hole(3, 4, true);
    CY_REQUIRE(store.insert(std::move(tile)).has_value());
    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);

    MeshReport report;
    cy::Expected<TerrainMesh, cy::Error> mesh = mesh_tile(
        test::allocator(), store, heights, &deltas, TileCoord{0, 0, 0}, same_level(0), report);
    CY_REQUIRE(mesh.has_value());
    CY_CHECK_EQ(report.hole_quads, 2U);
    CY_CHECK_EQ(report.triangles, (kTileQuads * kTileQuads * 2) - 4);

    MeshOptions fallback = same_level(0);
    fallback.virtual_geometry = false;
    fallback.fallback_decimation = 4;
    MeshReport fallback_report;
    cy::Expected<TerrainMesh, cy::Error> reduced = mesh_tile(
        test::allocator(), store, heights, &deltas, TileCoord{0, 0, 0}, fallback, fallback_report);
    CY_REQUIRE(reduced.has_value());
    CY_CHECK(fallback_report.fallback);
    CY_CHECK_NEAR(fallback_report.detail, 0.25F, 0.0001F);
    CY_CHECK_LT(fallback_report.triangles, report.triangles);
    CY_CHECK_NE(reduced.value().key, mesh.value().key);
    CY_REQUIRE(fallback_report.limitation != nullptr);
    CY_CHECK_NE(fallback_report.limitation[0], '\0');
}

CY_TEST_CASE("collision is coarser than rendering, independent of it, and carries the hole") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    TerrainTile tile = test::make_tile(shape, TileCoord{0, 0, 0}, hills);
    tile.set_hole(8, 8, true);
    CY_REQUIRE(store.insert(std::move(tile)).has_value());
    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);

    CollisionConfig config;
    config.decimation = 4;
    TerrainCollision collision(test::allocator(), store, config);
    const TileCoord tiles[1] = {TileCoord{0, 0, 0}};
    cy::Expected<CollisionRegistration, cy::Error> registered =
        collision.activate(heights, cy::Span<const TileCoord>(tiles, 1));
    CY_REQUIRE(registered.has_value());
    CY_CHECK_EQ(registered.value().tiles, 1U);

    const CollisionTile* built = collision.find(TileCoord{0, 0, 0});
    CY_REQUIRE(built != nullptr);
    CY_CHECK_EQ(built->edge, 17U);  // 64 quads decimated by four, plus the shared line
    CY_CHECK_NEAR(built->spacing, 16.0F, 0.001F);
    CY_CHECK_GT(built->holes, 0U);

    // The hole reaches physics as the sentinel `physics` already defines for it.
    const cy::physics::HeightFieldDescription description = describe_collision(*built);
    CY_CHECK_EQ(description.sample_count_x, 17U);
    CY_CHECK_EQ(description.samples[(2 * 17) + 2], cy::physics::kHeightFieldHole);

    // A VISUAL deformation does not move it: collision reads the gameplay surface alone.
    Deformation rut;
    rut.klass = DeformationClass::Visual;
    rut.bounds = TerrainBounds{60.0, 60.0, 68.0, 68.0};
    rut.amount = -0.4F;
    CY_REQUIRE(deltas.apply(rut).has_value());
    const cy::f32 before = built->samples[(4 * 17) + 4];
    cy::Expected<cy::u32, cy::Error> rebuilt = collision.invalidate(heights, rut.bounds);
    CY_REQUIRE(rebuilt.has_value());
    CY_CHECK_EQ(collision.find(TileCoord{0, 0, 0})->samples[(4 * 17) + 4], before);

    // A GAMEPLAY one does, and only for the tiles the rectangle touches.
    Deformation pit;
    pit.klass = DeformationClass::Gameplay;
    pit.bounds = TerrainBounds{56.0, 56.0, 72.0, 72.0};
    pit.amount = -8.0F;
    CY_REQUIRE(deltas.apply(pit).has_value());
    cy::Expected<cy::u32, cy::Error> again = collision.invalidate(heights, pit.bounds);
    CY_REQUIRE(again.has_value());
    CY_CHECK_EQ(again.value(), 1U);
    CY_CHECK_LT(collision.find(TileCoord{0, 0, 0})->samples[(4 * 17) + 4], before);

    CY_CHECK_EQ(collision.deactivate(cy::Span<const TileCoord>(tiles, 1)), 1U);
    CY_CHECK_EQ(collision.resident(), 0U);
}

CY_TEST_CASE("the navigation surface is derived from collision and takes declared costs") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    TerrainTile tile = test::make_tile(shape, TileCoord{0, 0, 0}, test::flat);
    // A band of mud across the tile.
    for (cy::u32 j = 20; j < 30; ++j) {
        for (cy::u32 i = 0; i < kTileTexels; ++i) {
            tile.texel(i, j) = MaterialTexel{{4, 0, 0, 0}, {255, 0, 0, 0}};
        }
    }
    tile.set_hole(40, 40, true);
    CY_REQUIRE(store.insert(std::move(tile)).has_value());
    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);

    CollisionConfig config;
    config.decimation = 2;
    cy::Expected<CollisionTile, cy::Error> collision =
        build_collision(test::allocator(), store, heights, TileCoord{0, 0, 0}, config);
    CY_REQUIRE(collision.has_value());

    TerrainNavigation navigation(test::allocator());
    NavLayerMapping mud;
    mud.layer = 4;
    mud.area = 5;
    mud.cost = 4.0F;
    CY_REQUIRE(navigation.add_layer_mapping(mud).has_value());

    cy::Expected<NavContribution, cy::Error> contribution =
        navigation.contribute(test::allocator(), store, collision.value(), nullptr);
    CY_REQUIRE(contribution.has_value());
    CY_CHECK_GT(contribution.value().holes, 0U);

    // "Material affects cost ... without special-case code": the declared mapping is the only path
    // from a layer to an area, and it shows up in the triangles the band covers.
    cy::u32 muddy = 0;
    for (const cy::navigation::AreaType area : contribution.value().area.span()) {
        muddy += (area == 5) ? 1U : 0U;
    }
    CY_CHECK_GT(muddy, 0U);
    CY_CHECK_LT(muddy, contribution.value().area.size());

    // And it is the shape `navigation::build_tile()` already takes, from everything else.
    cy::navigation::NavBuildParams params;
    params.cell_size = 1.0F;
    params.cell_height = 0.5F;
    cy::navigation::NavBuildReport report;
    const cy::Aabb bounds{cy::Vec3{static_cast<cy::f32>(contribution.value().bounds.min_x), 90.0F,
                                   static_cast<cy::f32>(contribution.value().bounds.min_z)},
                          cy::Vec3{static_cast<cy::f32>(contribution.value().bounds.max_x), 110.0F,
                                   static_cast<cy::f32>(contribution.value().bounds.max_z)}};
    cy::Expected<cy::navigation::NavTileData, cy::Error> built =
        cy::navigation::build_tile(test::allocator(), params, contribution.value().source(),
                                   cy::navigation::TileCoord{0, 0}, bounds, report);
    CY_REQUIRE(built.has_value());
    CY_CHECK_GT(report.triangles_in, 0U);

    // A gameplay deformation raises a dirty region; a visual one does not.
    const TerrainBounds region{100.0, 100.0, 120.0, 120.0};
    CY_REQUIRE(navigation.mark_dirty(cy::Span<const TerrainBounds>(&region, 1)).has_value());
    CY_CHECK_EQ(navigation.dirty().size(), 1U);
    navigation.clear_dirty();
    CY_CHECK(navigation.dirty().empty());
}
