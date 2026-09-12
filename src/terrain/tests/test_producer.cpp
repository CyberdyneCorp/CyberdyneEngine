// Terrain as an environment-fields PRODUCER and CONSUMER: the claim, the refusal a second producer
// gets, the declared consumption the firewall validates, the soil field terrain publishes, and the
// cooked field tile it supplies. M10 task 2.1.
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL: `TerrainSystem::register_producer()` was changed to
// swallow the refusal from `FieldRegistry::claim()` and carry on, and "terrain claims soil, and a
// second producer of it is refused naming both" went red on
// `CY_REQUIRE_FALSE(second.register_producer(...).has_value())`. It was then restored. The sequence
// is reported in this milestone's `verified_failing`.

#include <cy/test/test.h>

#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/terrain/system.h>

#include "fixtures.h"

namespace test = cy::terrain::test;
using namespace cy::terrain;

namespace {

[[nodiscard]] bool contains(const char* text, const char* needle) noexcept {
    if (text == nullptr || needle == nullptr) {
        return false;
    }
    for (const char* start = text; *start != '\0'; ++start) {
        const char* a = start;
        const char* b = needle;
        while (*b != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

}  // namespace

CY_TEST_CASE("terrain claims soil, and a second producer of it is refused naming both") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    TerrainDeltaStore deltas(test::allocator(), shape);
    cy::environment::FieldRegistry registry(test::allocator());

    TerrainSystem terrain(test::allocator(), store, deltas);
    CY_REQUIRE(registry.declare(soil_field_declaration(4.0F)).has_value());
    CY_REQUIRE(terrain.register_producer(registry, "terrain.surface", 4.0F).has_value());
    CY_CHECK(terrain.produces_soil());

    // A second terrain — an editor preview, a second world — claiming the same field is refused,
    // and the refusal names the incumbent and the challenger. Terrain does not route around the
    // rule it is the first user of.
    TerrainSystem other(test::allocator(), store, deltas);
    const cy::Status second = other.register_producer(registry, "terrain.preview", 4.0F);
    CY_REQUIRE_FALSE(second.has_value());
    CY_CHECK_EQ(second.error().code, cy::ErrorCode::AlreadyExists);
    CY_CHECK(contains(second.error().message, "terrain.surface"));
    CY_CHECK(contains(second.error().message, "terrain.preview"));
    CY_CHECK_FALSE(other.produces_soil());
    CY_CHECK_EQ(registry.last_conflict().incumbent, "terrain.surface");
    CY_CHECK_EQ(registry.last_conflict().challenger, "terrain.preview");
}

CY_TEST_CASE("the soil field is a category, produced on the CPU, and gameplay-visible") {
    const cy::environment::FieldDeclaration declaration = soil_field_declaration(4.0F);
    // A category, because an interpolated soil index names nothing. The substrate refuses the other
    // pairing; this asserts terrain did not ask for it.
    CY_CHECK_EQ(declaration.type, cy::environment::FieldType::Category);
    CY_CHECK_EQ(declaration.interpolation, cy::environment::FieldInterpolation::Nearest);
    CY_CHECK_EQ(declaration.production, cy::environment::FieldProduction::Cpu);
    CY_CHECK(declaration.gameplay_visible());
    CY_CHECK_EQ(cy::environment::validate_declaration(declaration),
                cy::environment::DeclarationProblem::None);
}

CY_TEST_CASE("terrain declares what it reads, and a firewall crossing is a configuration error") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    TerrainDeltaStore deltas(test::allocator(), shape);
    cy::environment::FieldRegistry registry(test::allocator());

    // A field that exists only to modulate a shader.
    cy::environment::FieldDeclaration visual;
    visual.name = "test.terrain.shimmer";
    visual.unit = "fraction";
    visual.semantics = "a purely visual modulation";
    visual.levels[2] = cy::environment::FieldLevel{16.0F, true};
    visual.classification = cy::determinism::SimulationClass::Presentation;
    CY_REQUIRE(registry.declare(visual).has_value());

    MaterialRuleSet rules(test::allocator());
    MaterialRule rule;
    rule.name = "shimmer decides the ground";
    rule.input = RuleInput::Field;
    rule.field = visual.id();
    rule.layer = 1;
    CY_REQUIRE(rules.add_rule(rule).has_value());

    TerrainSystem terrain(test::allocator(), store, deltas);
    CY_REQUIRE(terrain.declare_consumption(registry, "terrain.materials", rules).has_value());

    // `FieldRegistry::validate()` reports it before a frame has run, which is the point of
    // declaring the consumption at all.
    cy::Array<cy::environment::FirewallViolation> violations(test::allocator());
    CY_REQUIRE(registry.validate(violations).has_value());
    CY_REQUIRE_EQ(violations.size(), 1U);
    CY_CHECK_EQ(violations[0].field, visual.id());
    CY_CHECK_EQ(violations[0].consumer, "terrain.materials");
}

CY_TEST_CASE("the soil terrain publishes is the terrain's own material") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    TerrainTile tile = test::make_tile(shape, TileCoord{0, 0, 0}, test::flat);
    for (cy::u32 j = 0; j < kTileTexels; ++j) {
        for (cy::u32 i = 0; i < kTileTexels; ++i) {
            tile.texel(i, j) =
                MaterialTexel{{(i < 8) ? cy::u8{6} : cy::u8{2}, 0, 0, 0}, {255, 0, 0, 0}};
        }
    }
    CY_REQUIRE(store.insert(std::move(tile)).has_value());

    TerrainDeltaStore deltas(test::allocator(), shape);
    cy::environment::FieldRegistry registry(test::allocator());
    TerrainSystem terrain(test::allocator(), store, deltas);
    CY_REQUIRE(terrain.register_producer(registry, "terrain.surface", 4.0F).has_value());

    cy::environment::FieldStore fields(test::allocator(), registry, test::partition());
    HeightfieldSource heights(store, &deltas);
    const TileCoord tiles[1] = {TileCoord{0, 0, 0}};
    cy::Expected<cy::u32, cy::Error> written =
        terrain.publish_soil(fields, heights, cy::Span<const TileCoord>(tiles, 1),
                             cy::environment::FieldResidency::Macro);
    CY_REQUIRE(written.has_value());
    CY_CHECK_GT(written.value(), 0U);

    // Texels 0..7 are layer 6, which is x below 32 m at 4 m per texel.
    const cy::environment::FieldSample low =
        fields.sample_deterministic(terrain.soil(), cy::world::WorldVec3d{10.0, 0.0, 10.0});
    CY_CHECK_EQ(low.value.index(), 6U);
    const cy::environment::FieldSample high =
        fields.sample_deterministic(terrain.soil(), cy::world::WorldVec3d{200.0, 0.0, 10.0});
    CY_CHECK_EQ(high.value.index(), 2U);
}

CY_TEST_CASE("the cooked soil loader refuses a tile with no terrain under it") {
    const TileLayout shape = test::layout();
    TerrainStore store(test::allocator(), shape);
    CY_REQUIRE(store.insert(test::make_tile(shape, TileCoord{0, 0, 0}, test::flat)).has_value());

    cy::environment::FieldRegistry registry(test::allocator());
    const cy::environment::FieldDeclaration declaration = soil_field_declaration(4.0F);
    CY_REQUIRE(registry.declare(declaration).has_value());

    SoilLoaderContext context;
    context.store = &store;
    context.registry = &registry;

    cy::environment::TileAddress address;
    address.field = declaration.id();
    address.level = 0;
    cy::Array<cy::u8> bytes(test::allocator());
    CY_REQUIRE(terrain_soil_loader(&context, address, bytes).has_value());
    CY_CHECK_EQ(bytes.size(), cy::environment::FieldStore::tile_bytes(declaration));

    // A tile far outside the cooked terrain has no cooked data, and the loader says so rather than
    // handing the substrate a tile of invented values.
    address.x = 5000;
    address.z = 5000;
    CY_CHECK_FALSE(terrain_soil_loader(&context, address, bytes).has_value());
}
