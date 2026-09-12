// The cook side: a terrain tile through the ENGINE'S virtual geometry builder, and the terrain
// surface material through the ENGINE'S material compiler. M10 task 2.1.
//
// The two claims here are negative ones — "there SHALL NOT be a terrain-specific renderer" and "a
// monolithic terrain shader carrying every layer and every feature SHALL NOT be produced" — and a
// negative claim is only checkable against the thing it denies. So both tests do the same shape of
// work: run the engine's own call on terrain's input, and measure that the result does not scale
// with the thing the specification says it must not scale with.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: `build_terrain_material()` was changed to emit one
// closure per `layers_in_world` instead of per `layers_per_texel`, and "shading does not scale with
// the world's layer count" went red on the node-count comparison — 4 layers produced 13 nodes and
// 40 produced 121. It was then restored. The sequence is reported in this milestone's
// `verified_failing`.

#include <cy/test/test.h>

#include <cy/core/values/name.h>
#include <cy/terrain/cook.h>

#include "fixtures.h"

namespace test = cy::terrain::test;
using namespace cy::terrain;

namespace {

[[nodiscard]] cy::f32 dunes(cy::f64 x, cy::f64 z) noexcept {
    return 100.0F +
           (8.0F * static_cast<cy::f32>((x * 0.017) -
                                        static_cast<cy::f64>(static_cast<cy::i64>(x * 0.017)))) +
           (5.0F * static_cast<cy::f32>((z * 0.029) -
                                        static_cast<cy::f64>(static_cast<cy::i64>(z * 0.029))));
}

}  // namespace

CY_TEST_CASE("a terrain tile becomes virtual geometry through the engine's own builder") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 0}, dunes)).has_value());
    TerrainDeltaStore deltas(test::allocator(), shape);
    HeightfieldSource heights(store, &deltas);

    MeshOptions options;
    for (unsigned char& edge : options.neighbour_level) {
        edge = 0;
    }
    MeshReport report;
    cy::Expected<TerrainMesh, cy::Error> mesh =
        mesh_tile(test::allocator(), store, heights, &deltas, TileCoord{0, 0, 0}, options, report);
    CY_REQUIRE(mesh.has_value());

    const cy::rendering::vg::SourceMesh source = source_mesh_of(mesh.value());
    CY_CHECK_EQ(source.positions.size(), mesh.value().positions.size());
    CY_CHECK_EQ(source.indices.size(), mesh.value().indices.size());

    cy::rendering::vg::BuildOptions build;
    build.policy = cy::rendering::vg::ClusterPolicy{};
    cy::Expected<cy::rendering::vg::GeometryBuild, cy::Error> built =
        build_tile_geometry(test::allocator(), mesh.value(), build);
    CY_REQUIRE(built.has_value());

    // Clusters, groups and pages, from the same builder a static mesh uses. There is no terrain
    // cluster type, no terrain page format and no terrain level-of-detail scheme, because there is
    // nowhere in this module for one to be.
    CY_CHECK_GT(built.value().clusters.size(), 1U);
    CY_CHECK_GT(built.value().groups.size(), 0U);
    CY_CHECK_GT(built.value().cluster_children.size(), 0U);

    // Deterministic: the same tile builds the same hierarchy, which is what a derivation key over a
    // terrain tile is only worth computing if.
    cy::Expected<cy::rendering::vg::GeometryBuild, cy::Error> again =
        build_tile_geometry(test::allocator(), mesh.value(), build);
    CY_REQUIRE(again.has_value());
    CY_REQUIRE_EQ(again.value().clusters.size(), built.value().clusters.size());
    for (cy::usize index = 0; index < built.value().positions.size(); ++index) {
        CY_REQUIRE_EQ(again.value().positions[index].y, built.value().positions[index].y);
    }
}

CY_TEST_CASE("shading does not scale with the world's layer count") {
    const cy::Name fields[2] = {
        cy::Name::intern("wetness"),
        cy::Name::intern("snow-depth"),
    };

    TerrainMaterialDescription small;
    small.layers_per_texel = 4;
    small.fields = cy::Span<const cy::Name>(fields, 2);
    small.layers_in_world = 4;

    TerrainMaterialDescription large = small;
    large.layers_in_world = 40;

    cy::rendering::material::MaterialGraph few(test::allocator(),
                                               cy::Name::intern("terrain.surface.few"));
    cy::rendering::material::MaterialGraph many(test::allocator(),
                                                cy::Name::intern("terrain.surface.many"));
    CY_REQUIRE(build_terrain_material(few, small).has_value());
    CY_REQUIRE(build_terrain_material(many, large).has_value());

    // A terrain with forty layers authors the SAME graph as one with four: the texel says which
    // four it blends and the shader blends four. "shading cost SHALL not scale with the layer count
    // of the region."
    CY_CHECK_EQ(few.nodes().size(), many.nodes().size());

    // Raising the per-texel BOUND is what costs more, and it is the only thing that does.
    TerrainMaterialDescription narrow = small;
    narrow.layers_per_texel = 2;
    cy::rendering::material::MaterialGraph cheaper(test::allocator(),
                                                   cy::Name::intern("terrain.surface.narrow"));
    CY_REQUIRE(build_terrain_material(cheaper, narrow).has_value());
    CY_CHECK_LT(cheaper.nodes().size(), few.nodes().size());

    // A bound above the storage bound is refused rather than truncated.
    TerrainMaterialDescription impossible = small;
    impossible.layers_per_texel = kMaxTexelLayers + 1;
    cy::rendering::material::MaterialGraph refused(test::allocator(),
                                                   cy::Name::intern("terrain.surface.bad"));
    CY_CHECK_FALSE(build_terrain_material(refused, impossible).has_value());
}

CY_TEST_CASE("the terrain material compiles like any other, and its field reads are checked") {
    const cy::Name fields[1] = {cy::Name::intern("wetness")};
    TerrainMaterialDescription description;
    description.layers_per_texel = 2;
    description.fields = cy::Span<const cy::Name>(fields, 1);

    cy::rendering::material::MaterialGraph graph(test::allocator(),
                                                 cy::Name::intern("terrain.surface"));
    CY_REQUIRE(build_terrain_material(graph, description).has_value());

    cy::Expected<cy::rendering::material::Module, cy::Error> lowered =
        cy::rendering::material::lower_graph(graph, test::allocator());
    CY_REQUIRE(lowered.has_value());

    cy::rendering::material::CompileOptions options;
    const cy::Name declared[1] = {cy::Name::intern("wetness")};
    options.declared_fields = cy::Span<const cy::Name>(declared, 1);
    options.check_fields = true;

    cy::Expected<cy::rendering::material::CompiledMaterial, cy::Error> compiled =
        cy::rendering::material::compile_material(lowered.value(), options, test::allocator());
    CY_REQUIRE(compiled.has_value());
    CY_CHECK_FALSE(compiled.value().failed());
    CY_CHECK_GT(compiled.value().programs().size(), 0U);
    CY_CHECK_NE(compiled.value().cook_key(), 0U);

    // A field the project has not declared fails to cook naming it, "rather than silently
    // substituting a default". Terrain's environment inputs go through the compiler's own check,
    // not through a terrain-specific one.
    cy::rendering::material::CompileOptions undeclared = options;
    const cy::Name empty[1] = {cy::Name::intern("moisture")};
    undeclared.declared_fields = cy::Span<const cy::Name>(empty, 1);
    cy::Expected<cy::rendering::material::CompiledMaterial, cy::Error> refused =
        cy::rendering::material::compile_material(lowered.value(), undeclared, test::allocator());
    CY_REQUIRE(refused.has_value());
    CY_CHECK(refused.value().failed());
}
