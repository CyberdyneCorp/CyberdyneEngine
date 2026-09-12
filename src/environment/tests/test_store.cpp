// The sparse tiled store: what an empty region costs, what a sample outside resident data returns,
// the layer rule, versions and change events, the batched call, and the diagnostics. Task 1.1.

#include <cy/test/test.h>

#include <cy/environment/store.h>

#include "fixtures.h"

using cy::environment::FieldChange;
using cy::environment::FieldChangeKind;
using cy::environment::FieldLayer;
using cy::environment::FieldRegistry;
using cy::environment::FieldResidency;
using cy::environment::FieldSample;
using cy::environment::FieldStore;
using cy::environment::FieldValue;
using cy::environment::ProducerKind;
using cy::environment::ProducerToken;
using cy::environment::TileAddress;
namespace test = cy::environment::test;

namespace {

/// A registry, a store and a claimed token for one declaration. Every case needs the three and
/// none of them is the thing under test.
struct Fixture {
    FieldRegistry registry;
    cy::world::PartitionConfig partition;
    FieldStore store;
    cy::environment::FieldId field;

    explicit Fixture(const cy::environment::FieldDeclaration& declaration) noexcept
        : registry(test::allocator()),
          partition(test::partition()),
          store(test::allocator(), registry, partition),
          field(declaration.id()) {
        CY_REQUIRE(registry.declare(declaration).has_value());
    }

    [[nodiscard]] ProducerToken claim(const char* producer) noexcept {
        cy::Expected<ProducerToken, cy::Error> token =
            registry.claim(field, producer, ProducerKind::System);
        CY_REQUIRE(token.has_value());
        return std::move(*token);
    }
};

}  // namespace

CY_TEST_CASE("an empty region costs nothing and samples the declared default") {
    // "WHEN a field has data only near the surface, THEN empty regions SHALL consume no storage",
    // and "WHEN a system samples a field in an unloaded region, THEN it SHALL receive the coarsest
    // resident value or the declared default, without blocking."
    Fixture fixture(test::moisture_like());
    CY_CHECK_EQ(fixture.store.tile_count(), 0u);
    CY_CHECK_EQ(fixture.store.bytes_resident(), 0u);

    const FieldSample sample = fixture.store.sample(fixture.field, test::at(9'000.0, -4'000.0));
    CY_CHECK_FALSE(sample.resolved);
    CY_CHECK_EQ(sample.value.x(), 0.25F);  // the declaration's default
    CY_CHECK_EQ(fixture.store.tile_count(), 0u);
}

CY_TEST_CASE("a sample takes the finest resident level and says which one it was") {
    // "A sample SHALL return the finest resident level at that position together with a resolution
    // indicator, so a consumer can decide whether the answer is precise enough."
    Fixture fixture(test::moisture_like());
    const ProducerToken token = fixture.claim("terrain.hydrology");

    CY_REQUIRE(test::fill_tile(fixture.store, token, test::tile_at(fixture.field, 2, 0, 0),
                               FieldValue::scalar(0.5F))
                   .has_value());
    const FieldSample coarse = fixture.store.sample(fixture.field, test::at(10.0, 10.0));
    CY_CHECK(coarse.resolved);
    CY_CHECK(coarse.level == FieldResidency::Macro);
    CY_CHECK_EQ(coarse.cell_metres, 64.0F);
    CY_CHECK_NEAR(coarse.value.x(), 0.5F, 1.0F / 255.0F);

    // A local tile over the same position wins, and the indicator says so.
    CY_REQUIRE(test::fill_tile(fixture.store, token, test::tile_at(fixture.field, 0, 0, 0),
                               FieldValue::scalar(0.9F))
                   .has_value());
    const FieldSample fine = fixture.store.sample(fixture.field, test::at(10.0, 10.0));
    CY_CHECK(fine.level == FieldResidency::Local);
    CY_CHECK_EQ(fine.cell_metres, 2.0F);
    CY_CHECK_NEAR(fine.value.x(), 0.9F, 1.0F / 255.0F);

    // Twenty metres away is outside the local tile (16 cells of 2 m = 32 m) but inside the macro
    // one, so the answer is coarse rather than absent. A consumer that needs to know reads `level`.
    const FieldSample beyond = fixture.store.sample(fixture.field, test::at(100.0, 100.0));
    CY_CHECK(beyond.resolved);
    CY_CHECK(beyond.level == FieldResidency::Macro);
}

CY_TEST_CASE("the declared layer rule combines a baked base and a runtime delta") {
    // "WHEN a field has a baked base and a runtime delta, THEN the combination rule SHALL be part
    // of its declaration and SHALL be applied consistently by every reader."
    cy::environment::FieldDeclaration declaration = test::moisture_like();
    declaration.name = "test.snow-depth";
    declaration.layer_rule = cy::environment::FieldLayerRule::Add;
    declaration.range_max = 4.0F;
    declaration.default_value = FieldValue::scalar(0.0F);
    Fixture fixture(declaration);
    const ProducerToken token = fixture.claim("weather.snow");

    CY_REQUIRE(test::fill_tile(fixture.store, token,
                               test::tile_at(fixture.field, 2, 0, 0, FieldLayer::Base),
                               FieldValue::scalar(1.0F))
                   .has_value());
    CY_CHECK_NEAR(fixture.store.sample(fixture.field, test::at(10.0, 10.0)).value.x(), 1.0F,
                  4.0F / 255.0F);

    CY_REQUIRE(test::fill_tile(fixture.store, token,
                               test::tile_at(fixture.field, 2, 0, 0, FieldLayer::Delta),
                               FieldValue::scalar(0.5F))
                   .has_value());
    CY_CHECK_NEAR(fixture.store.sample(fixture.field, test::at(10.0, 10.0)).value.x(), 1.5F,
                  8.0F / 255.0F);

    // The point query names both contributions and the producer — "a point query showing the value
    // and which layer contributed it".
    const cy::environment::FieldPointQuery query =
        fixture.store.point_query(fixture.field, test::at(10.0, 10.0));
    CY_CHECK(query.has_base);
    CY_CHECK(query.has_delta);
    CY_CHECK_NEAR(query.base.x(), 1.0F, 4.0F / 255.0F);
    CY_CHECK_NEAR(query.delta.x(), 0.5F, 4.0F / 255.0F);
    CY_CHECK(test::same_text(query.producer, "weather.snow"));
    CY_CHECK(test::same_text(query.field_name, "test.snow-depth"));
}

CY_TEST_CASE("a category is read exactly, because nothing averages it") {
    Fixture fixture(test::biome_like());
    const ProducerToken token = fixture.claim("pcg.biome");
    const TileAddress tile = test::tile_at(fixture.field, 0, 0, 0);

    cy::Expected<cy::environment::FieldWriter, cy::Error> writer = fixture.store.open_writer(token);
    CY_REQUIRE(writer.has_value());
    CY_REQUIRE(writer->stage(tile).has_value());
    CY_REQUIRE(writer->fill(tile, FieldValue::category(7)).has_value());
    CY_REQUIRE(writer->set(tile, 3, 0, 4, FieldValue::category(19)).has_value());
    CY_REQUIRE(writer->publish().has_value());

    // Level 0 cells are 8 m: lattice point (3, 4) is the cell [24,32) x [32,40).
    CY_CHECK_EQ(fixture.store.sample(fixture.field, test::at(28.0, 36.0)).value.index(), 19u);
    CY_CHECK_EQ(fixture.store.sample(fixture.field, test::at(4.0, 4.0)).value.index(), 7u);
    // And no value between them exists: a nearest sample of a category is one stored number.
    CY_CHECK_EQ(fixture.store.sample(fixture.field, test::at(31.9, 36.0)).value.index(), 19u);
    CY_CHECK_EQ(fixture.store.sample(fixture.field, test::at(32.1, 36.0)).value.index(), 7u);
}

CY_TEST_CASE("a volumetric field varies with height, and a planar one is the same code path") {
    Fixture fixture(test::wind_like());
    const ProducerToken token = fixture.claim("weather.wind");
    const TileAddress tile = test::tile_at(fixture.field, 0, 0, 0);

    cy::Expected<cy::environment::FieldWriter, cy::Error> writer = fixture.store.open_writer(token);
    CY_REQUIRE(writer.has_value());
    CY_REQUIRE(writer->stage(tile).has_value());
    // Four columns of 25 m. Ground is still; the top blows hard.
    for (cy::u32 z = 0; z < cy::environment::kTileCells; ++z) {
        for (cy::u32 x = 0; x < cy::environment::kTileCells; ++x) {
            for (cy::u32 y = 0; y < 4; ++y) {
                CY_REQUIRE(writer
                               ->set(tile, x, y, z,
                                     FieldValue::vec3(static_cast<cy::f32>(y) * 4.0F, 0.0F, 0.0F))
                               .has_value());
            }
        }
    }
    CY_REQUIRE(writer->publish().has_value());

    // Cell centres sit at 12.5, 37.5, 62.5 and 87.5 m.
    CY_CHECK_NEAR(fixture.store.sample(fixture.field, test::at(10.0, 10.0, 12.5)).value.x(), 0.0F,
                  1.0e-4F);
    CY_CHECK_NEAR(fixture.store.sample(fixture.field, test::at(10.0, 10.0, 37.5)).value.x(), 4.0F,
                  1.0e-4F);
    // Halfway between two columns is the blend of both, which is what makes this one sampling path
    // rather than a planar one with a volumetric special case beside it.
    CY_CHECK_NEAR(fixture.store.sample(fixture.field, test::at(10.0, 10.0, 25.0)).value.x(), 2.0F,
                  1.0e-4F);
    // Below the column and above it, the value is clamped rather than extrapolated or faulted.
    CY_CHECK_NEAR(fixture.store.sample(fixture.field, test::at(10.0, 10.0, -50.0)).value.x(), 0.0F,
                  1.0e-4F);
    CY_CHECK_NEAR(fixture.store.sample(fixture.field, test::at(10.0, 10.0, 500.0)).value.x(), 12.0F,
                  1.0e-4F);
}

CY_TEST_CASE("a region carries a version, and a change event names its bounds") {
    // "Every field region SHALL carry a version that increments when its values change", and
    // "Change events SHALL be raised at region granularity with the changed bounds".
    Fixture fixture(test::moisture_like());
    const ProducerToken token = fixture.claim("terrain.hydrology");

    cy::Expected<cy::environment::FieldChangeQueue::ConsumerId, cy::Error> consumer =
        fixture.store.changes().add_consumer("pcg", 0);
    CY_REQUIRE(consumer.has_value());

    const TileAddress tile = test::tile_at(fixture.field, 2, 1, 1);
    CY_REQUIRE(test::fill_tile(fixture.store, token, tile, FieldValue::scalar(0.5F)).has_value());
    const cy::u64 first = fixture.store.version_of(tile);
    CY_CHECK_GT(first, 0u);

    CY_REQUIRE(test::fill_tile(fixture.store, token, tile, FieldValue::scalar(0.6F)).has_value());
    // "WHEN a consumer holds a derived value, THEN it SHALL detect staleness by version comparison
    // rather than by re-reading the field."
    CY_CHECK_GT(fixture.store.version_of(tile), first);

    cy::Array<FieldChange> drained(test::allocator());
    CY_REQUIRE(fixture.store.changes().drain(consumer.value(), drained).has_value());
    CY_REQUIRE_EQ(drained.size(), 2u);
    CY_CHECK(drained[0].kind == FieldChangeKind::Values);
    CY_CHECK(drained[0].address == tile);
    // A macro tile is 16 cells of 64 m: tile (1,1) is [1024, 2048) on both axes, and the consumer
    // invalidating a rectangle gets that rectangle rather than having to derive it.
    CY_CHECK_EQ(drained[0].bounds.min_x, 1024.0);
    CY_CHECK_EQ(drained[0].bounds.max_x, 2048.0);
    CY_CHECK_EQ(drained[0].bounds.min_z, 1024.0);
    CY_CHECK_LT(drained[0].sequence, drained[1].sequence);

    // Drained once. A second drain reports nothing new rather than the whole history.
    cy::Array<FieldChange> again(test::allocator());
    CY_REQUIRE(fixture.store.changes().drain(consumer.value(), again).has_value());
    CY_CHECK_EQ(again.size(), 0u);
}

CY_TEST_CASE("a write is visible when it is published, not while it is being made") {
    // "writes SHALL be visible to consumers on a defined schedule rather than immediately
    // mid-frame". A consumer sampling while a producer is writing sees the previous values
    // everywhere rather than half of each.
    Fixture fixture(test::moisture_like());
    const ProducerToken token = fixture.claim("terrain.hydrology");
    const TileAddress tile = test::tile_at(fixture.field, 2, 0, 0);
    CY_REQUIRE(test::fill_tile(fixture.store, token, tile, FieldValue::scalar(0.2F)).has_value());

    cy::Expected<cy::environment::FieldWriter, cy::Error> writer = fixture.store.open_writer(token);
    CY_REQUIRE(writer.has_value());
    CY_REQUIRE(writer->stage(tile).has_value());
    CY_REQUIRE(writer->fill(tile, FieldValue::scalar(0.8F)).has_value());
    CY_CHECK_EQ(writer->staged(), 1u);

    // Staged, not published: the consumer still sees the old world.
    CY_CHECK_NEAR(fixture.store.sample(fixture.field, test::at(10.0, 10.0)).value.x(), 0.2F,
                  1.0F / 255.0F);
    CY_REQUIRE(writer->publish().has_value());
    CY_CHECK_NEAR(fixture.store.sample(fixture.field, test::at(10.0, 10.0)).value.x(), 0.8F,
                  1.0F / 255.0F);
    CY_CHECK_EQ(writer->staged(), 0u);

    // Staging an existing tile copies its values in, so editing one lattice point does not erase
    // the rest.
    cy::Expected<cy::environment::FieldWriter, cy::Error> second = fixture.store.open_writer(token);
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(second->stage(tile).has_value());
    CY_REQUIRE(second->set(tile, 0, 0, 0, FieldValue::scalar(0.1F)).has_value());
    CY_REQUIRE(second->publish().has_value());
    CY_CHECK_NEAR(fixture.store.sample(fixture.field, test::at(900.0, 900.0)).value.x(), 0.8F,
                  1.0F / 255.0F);
}

CY_TEST_CASE("ten thousand positions in one call agree with ten thousand calls") {
    // "WHEN a system samples a field at ten thousand positions, THEN it SHALL be able to do so in
    // one batched call." A hundred here, because the unit budget is a millisecond; the batched path
    // is the same code either way and `environment_gpu` samples in the thousands.
    Fixture fixture(test::moisture_like());
    const ProducerToken token = fixture.claim("terrain.hydrology");
    CY_REQUIRE(test::fill_tile(fixture.store, token, test::tile_at(fixture.field, 0, 0, 0),
                               FieldValue::scalar(0.75F))
                   .has_value());

    cy::Array<cy::world::WorldVec3d> positions(test::allocator());
    for (cy::u32 index = 0; index < 100; ++index) {
        CY_REQUIRE(positions
                       .push_back(test::at(static_cast<cy::f64>(index) * 0.3,
                                           static_cast<cy::f64>(index) * 0.2))
                       .has_value());
    }
    cy::Array<FieldSample> batched(test::allocator());
    for (cy::u32 index = 0; index < 100; ++index) {
        CY_REQUIRE(batched.push_back(FieldSample{}).has_value());
    }
    CY_REQUIRE(
        fixture.store.sample_many(fixture.field, positions.span(), batched.span()).has_value());

    for (cy::usize index = 0; index < positions.size(); ++index) {
        const FieldSample single = fixture.store.sample(fixture.field, positions[index]);
        CY_CHECK_EQ(batched[index].value.x(), single.value.x());
        CY_CHECK(batched[index].level == single.level);
        CY_CHECK_EQ(batched[index].resolved, single.resolved);
    }

    // An output span shorter than the positions is refused rather than truncated.
    CY_CHECK_FALSE(
        fixture.store
            .sample_many(fixture.field, positions.span(), cy::Span<FieldSample>(batched.data(), 4))
            .has_value());
}

CY_TEST_CASE("a tile is evicted and its bytes go, and a guaranteed one refuses") {
    Fixture fixture(test::moisture_like());
    const ProducerToken token = fixture.claim("terrain.hydrology");
    const TileAddress local = test::tile_at(fixture.field, 0, 0, 0);
    const TileAddress macro = test::tile_at(fixture.field, 2, 0, 0);

    CY_REQUIRE(fixture.store.insert_default_tile(token, local, /*guaranteed=*/false).has_value());
    CY_REQUIRE(fixture.store.insert_default_tile(token, macro, /*guaranteed=*/true).has_value());
    const cy::u64 bytes = fixture.store.bytes_resident();
    CY_CHECK_EQ(bytes, 2u * FieldStore::tile_bytes(test::moisture_like()));

    CY_REQUIRE(fixture.store.evict_tile(token, local).has_value());
    CY_CHECK_FALSE(fixture.store.is_resident(local));
    CY_CHECK_EQ(fixture.store.bytes_resident(), bytes / 2);

    // "Macro-level data SHALL be resident for the whole world where a field declares it." The
    // guaranteed tile is the coarse fallback every other answer is defined in terms of, so the
    // eviction is refused rather than honoured and reported afterwards.
    CY_CHECK_FALSE(fixture.store.evict_tile(token, macro).has_value());
    CY_CHECK(fixture.store.is_resident(macro));

    // The index survives the removal: the tile that was moved to fill the hole is still findable.
    CY_CHECK_EQ(fixture.store.version_of(macro), 1u);
}

CY_TEST_CASE("the diagnostics report the producer, the extents, the memory and the sample cost") {
    // "The engine SHALL provide, per field: a visualisation over the world, resident tile extents
    // and resolutions, memory in use, producer identity, sample cost, and a point query."
    Fixture fixture(test::moisture_like());
    const ProducerToken token = fixture.claim("terrain.hydrology");
    CY_REQUIRE(
        fixture.store.insert_default_tile(token, test::tile_at(fixture.field, 0, -2, 3), false)
            .has_value());
    CY_REQUIRE(
        fixture.store.insert_default_tile(token, test::tile_at(fixture.field, 0, 5, -1), false)
            .has_value());
    CY_REQUIRE(fixture.store.insert_default_tile(token, test::tile_at(fixture.field, 2, 0, 0), true)
                   .has_value());

    const FieldSample ignored = fixture.store.sample(fixture.field, test::at(10.0, 10.0));
    (void)ignored;

    const cy::environment::FieldDiagnostics report = fixture.store.diagnostics(fixture.field);
    CY_CHECK(test::same_text(report.producer, "terrain.hydrology"));
    CY_CHECK(test::same_text(report.field_name, "test.moisture"));
    CY_CHECK_EQ(report.tiles[0], 2u);
    CY_CHECK_EQ(report.tiles[2], 1u);
    CY_CHECK_EQ(report.bytes, 3u * FieldStore::tile_bytes(test::moisture_like()));
    CY_CHECK_EQ(report.min_tile_x, -2);
    CY_CHECK_EQ(report.max_tile_x, 5);
    CY_CHECK_EQ(report.min_tile_z, -1);
    CY_CHECK_EQ(report.max_tile_z, 3);
    // Sample cost, counted rather than estimated: one sample, and the lattice points it read.
    CY_CHECK_EQ(report.samples, 1u);
    CY_CHECK_GT(report.lattice_reads, 0u);
}

CY_TEST_CASE(
    "current state recovers toward potential, and a burned forest is still forest country") {
    // "WHEN a forest burns, THEN its current vegetation state SHALL drop while its potential
    // remains, so it regrows as forest rather than becoming permanent grassland."
    cy::environment::FieldDeclaration potential = test::moisture_like();
    potential.name = "test.vegetation-potential";
    potential.default_value = FieldValue::scalar(0.0F);

    cy::environment::FieldDeclaration current = test::moisture_like();
    current.name = "test.vegetation";
    current.default_value = FieldValue::scalar(0.0F);
    current.potential = potential.id();
    current.recovery_per_second = 0.5F;

    FieldRegistry registry(test::allocator());
    const cy::world::PartitionConfig partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);
    CY_REQUIRE(registry.declare(potential).has_value());
    CY_REQUIRE(registry.declare(current).has_value());

    cy::Expected<ProducerToken, cy::Error> potential_token =
        registry.claim(potential.id(), "climate", ProducerKind::System);
    cy::Expected<ProducerToken, cy::Error> current_token =
        registry.claim(current.id(), "ecology", ProducerKind::System);
    CY_REQUIRE(potential_token.has_value());
    CY_REQUIRE(current_token.has_value());

    const TileAddress here = test::tile_at(potential.id(), 2, 0, 0);
    CY_REQUIRE(
        test::fill_tile(store, *potential_token, here, FieldValue::scalar(1.0F)).has_value());
    // The fire: current state drops, and the producer writes the drop.
    CY_REQUIRE(test::fill_tile(store, *current_token, test::tile_at(current.id(), 2, 0, 0),
                               FieldValue::scalar(0.0F))
                   .has_value());

    CY_REQUIRE(store.advance_recovery(*current_token, 1.0F).has_value());
    const cy::f32 after_one = store.sample(current.id(), test::at(10.0, 10.0)).value.x();
    CY_CHECK_GT(after_one, 0.3F);
    CY_CHECK_LT(after_one, 0.5F);

    CY_REQUIRE(store.advance_recovery(*current_token, 4.0F).has_value());
    const cy::f32 later = store.sample(current.id(), test::at(10.0, 10.0)).value.x();
    CY_CHECK_GT(later, after_one);
    // It approaches the potential and does not overshoot it: the recovery closes a fraction of the
    // remaining gap rather than adding a rate.
    CY_CHECK_LE(later, 1.0F);
}
