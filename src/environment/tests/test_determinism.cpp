// Determinism of gameplay-visible fields, and the firewall for presentation-only ones. Task 1.4.
//
// design.md §3: "M10 adds no second mechanism." Everything here goes through
// `simulation-and-determinism`'s own classification and its own `may_read()` predicate; what this
// module adds is two refusals that make the classification MEAN something for a substrate that is
// streamed:
//
//   * a gameplay-visible field may not be produced on the GPU, and
//   * a gameplay-visible field's gameplay level must be resident everywhere,
//
// because a value that is fine near the player and coarse away from them depends on where the
// player is, which is the definition of not deterministic.
//
// HOW THE THREE CLAIMS HERE WERE SHOWN TO BE ABLE TO FAIL — each break run, watched, and restored:
//
//   * `sample_deterministic()` was changed to call `sample()`, the finest-resident walk. "A
//     gameplay-visible sample does not move when a finer tile streams in" went red after the local
//     tile is inserted, reporting 0.901961 where 0.501961 was required, and "every reader of one
//     field observes one value" went red with it.
//   * `FieldReader::open()`'s `may_read()` call was replaced by `if (false)`. "The firewall refuses
//     a gameplay reader of a visual field" went red on all three refusals at once.
//   * `FieldRegistry::validate()`'s `may_read()` call was replaced by `if (true)`. "Configuration
//     validation names the crossing" went red reporting 0 violations where 1 was required.
//
// Those are this milestone's `verified_failing` for section 1.4.

#include <cy/test/test.h>

#include <cy/environment/store.h>

#include "fixtures.h"

using cy::determinism::SimulationClass;
using cy::environment::DeclarationProblem;
using cy::environment::FieldDeclaration;
using cy::environment::FieldLevel;
using cy::environment::FieldProduction;
using cy::environment::FieldReader;
using cy::environment::FieldRegistry;
using cy::environment::FieldResidency;
using cy::environment::FieldSample;
using cy::environment::FieldStore;
using cy::environment::FieldValue;
using cy::environment::ProducerKind;
using cy::environment::ProducerToken;
namespace test = cy::environment::test;

CY_TEST_CASE("a gameplay-visible field may not be produced by a GPU pass") {
    // "A deterministic field SHALL NOT be produced by a GPU pass whose result is not read back
    // deterministically; such a field SHALL be either CPU-produced or declared visual."
    FieldDeclaration declaration = test::moisture_like();
    declaration.production = FieldProduction::Gpu;
    CY_CHECK(cy::environment::validate_declaration(declaration) ==
             DeclarationProblem::GpuAuthoritative);

    FieldRegistry registry(test::allocator());
    CY_CHECK_FALSE(registry.declare(declaration).has_value());

    // Declared visual, the same field is accepted: the requirement is a choice between two
    // declarations rather than a prohibition on GPU production.
    declaration.classification = SimulationClass::Presentation;
    CY_CHECK(cy::environment::validate_declaration(declaration) == DeclarationProblem::None);
    CY_CHECK(registry.declare(declaration).has_value());
}

CY_TEST_CASE("a gameplay-visible field's gameplay level must exist everywhere") {
    // The level gameplay reads must be one the world guarantees, or the value gameplay sees is a
    // function of what streamed. Refused at declaration rather than checked at every sample.
    FieldDeclaration declaration = test::moisture_like();
    declaration.gameplay_level = FieldResidency::Local;  // fine detail, streamed, not guaranteed
    CY_CHECK(cy::environment::validate_declaration(declaration) ==
             DeclarationProblem::GameplayLevelNotGuaranteed);

    declaration.levels[2] = FieldLevel{64.0F, false};
    declaration.gameplay_level = FieldResidency::Macro;
    CY_CHECK(cy::environment::validate_declaration(declaration) ==
             DeclarationProblem::GameplayLevelNotGuaranteed);

    // A presentation field has no such constraint: it is allowed to look better where it is loaded.
    declaration.classification = SimulationClass::Presentation;
    CY_CHECK(cy::environment::validate_declaration(declaration) == DeclarationProblem::None);
}

CY_TEST_CASE("a gameplay-visible sample does not move when a finer tile streams in") {
    // THE CASE THE WHOLE SPLIT EXISTS FOR. "its value at a position SHALL depend only on cooked
    // data, the world seed, and recorded persistent changes — never on frame timing, camera
    // position, STREAMING ORDER, or GPU execution order."
    FieldRegistry registry(test::allocator());
    const cy::world::PartitionConfig partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);
    const FieldDeclaration moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());
    cy::Expected<ProducerToken, cy::Error> token =
        registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(token.has_value());

    CY_REQUIRE(test::fill_tile(store, *token, test::tile_at(moisture.id(), 2, 0, 0),
                               FieldValue::scalar(0.5F))
                   .has_value());
    const cy::world::WorldVec3d where = test::at(10.0, 10.0);
    const cy::f32 before = store.sample_deterministic(moisture.id(), where).value.x();
    CY_CHECK_NEAR(before, 0.5F, 1.0F / 255.0F);

    // A local tile arrives — the player walked over here. The presentation answer improves.
    CY_REQUIRE(test::fill_tile(store, *token, test::tile_at(moisture.id(), 0, 0, 0),
                               FieldValue::scalar(0.9F))
                   .has_value());
    CY_CHECK_NEAR(store.sample(moisture.id(), where).value.x(), 0.9F, 1.0F / 255.0F);
    // The gameplay answer does not. This is the assertion that goes red if the gameplay path is
    // pointed at the finest-resident walk.
    CY_CHECK_EQ(store.sample_deterministic(moisture.id(), where).value.x(), before);

    // And it does not move back when the tile leaves, either: eviction is as much a streaming event
    // as arrival, and a value that changed on one would change on the other.
    CY_REQUIRE(store.evict_tile(*token, test::tile_at(moisture.id(), 0, 0, 0)).has_value());
    CY_CHECK_EQ(store.sample_deterministic(moisture.id(), where).value.x(), before);

    // A position with no data at the gameplay level is the declared default — a defined, streaming-
    // independent answer — rather than whatever coarser thing happens to be resident.
    const FieldSample far = store.sample_deterministic(moisture.id(), test::at(50'000.0, 50'000.0));
    CY_CHECK_FALSE(far.resolved);
    CY_CHECK_EQ(far.value.x(), moisture.default_value.x());
    CY_CHECK(far.level == FieldResidency::Macro);
}

CY_TEST_CASE("the order tiles arrive in does not change a gameplay-visible answer") {
    // Two stores fed the same tiles in opposite orders. Everything a peer's streaming can differ in
    // is order, so this is the property a networked session depends on.
    const FieldDeclaration moisture = test::moisture_like();
    const cy::world::PartitionConfig partition = test::partition();

    FieldRegistry first_registry(test::allocator());
    FieldStore first(test::allocator(), first_registry, partition);
    CY_REQUIRE(first_registry.declare(moisture).has_value());
    cy::Expected<ProducerToken, cy::Error> first_token =
        first_registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(first_token.has_value());

    FieldRegistry second_registry(test::allocator());
    FieldStore second(test::allocator(), second_registry, partition);
    CY_REQUIRE(second_registry.declare(moisture).has_value());
    cy::Expected<ProducerToken, cy::Error> second_token =
        second_registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(second_token.has_value());

    // The macro tile and three local ones, in opposite orders.
    CY_REQUIRE(test::fill_tile(first, *first_token, test::tile_at(moisture.id(), 2, 0, 0),
                               FieldValue::scalar(0.4F))
                   .has_value());
    for (cy::i32 index = 0; index < 3; ++index) {
        CY_REQUIRE(test::fill_tile(first, *first_token, test::tile_at(moisture.id(), 0, index, 0),
                                   FieldValue::scalar(0.9F))
                       .has_value());
    }
    for (cy::i32 index = 2; index >= 0; --index) {
        CY_REQUIRE(test::fill_tile(second, *second_token, test::tile_at(moisture.id(), 0, index, 0),
                                   FieldValue::scalar(0.9F))
                       .has_value());
    }
    CY_REQUIRE(test::fill_tile(second, *second_token, test::tile_at(moisture.id(), 2, 0, 0),
                               FieldValue::scalar(0.4F))
                   .has_value());

    for (cy::u32 step = 0; step < 24; ++step) {
        const cy::world::WorldVec3d where = test::at(static_cast<cy::f64>(step) * 3.7, 11.0);
        CY_CHECK_EQ(first.sample_deterministic(moisture.id(), where).value.x(),
                    second.sample_deterministic(moisture.id(), where).value.x());
    }
}

CY_TEST_CASE("the firewall refuses a gameplay reader of a visual field, at the open") {
    // "WHEN a field exists only to modulate a shader, THEN it SHALL be declared visual, and
    // gameplay SHALL be prevented from reading it." `determinism::may_read()` is the predicate,
    // used here and not re-implemented.
    FieldRegistry registry(test::allocator());
    const cy::world::PartitionConfig partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);
    const FieldDeclaration wind = test::wind_like();          // Presentation
    const FieldDeclaration moisture = test::moisture_like();  // Persistent
    CY_REQUIRE(registry.declare(wind).has_value());
    CY_REQUIRE(registry.declare(moisture).has_value());

    CY_CHECK_FALSE(FieldReader::open(store, wind.id(), SimulationClass::Authoritative).has_value());
    CY_CHECK_FALSE(FieldReader::open(store, wind.id(), SimulationClass::Persistent).has_value());
    // Derived is restricted to the authoritative set for the reason classification.h gives: a cache
    // computed from presentation data is a presentation cache.
    CY_CHECK_FALSE(FieldReader::open(store, wind.id(), SimulationClass::Derived).has_value());
    // Presentation reads anything. Appearance is allowed to look at everything.
    CY_CHECK(FieldReader::open(store, wind.id(), SimulationClass::Presentation).has_value());
    CY_CHECK(FieldReader::open(store, moisture.id(), SimulationClass::Authoritative).has_value());
    CY_CHECK(FieldReader::open(store, moisture.id(), SimulationClass::Presentation).has_value());
}

CY_TEST_CASE("configuration validation names the crossing before a frame has run") {
    FieldRegistry registry(test::allocator());
    const FieldDeclaration wind = test::wind_like();
    const FieldDeclaration moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(wind).has_value());
    CY_REQUIRE(registry.declare(moisture).has_value());

    CY_REQUIRE(
        registry.declare_consumer("foliage.placement", SimulationClass::Persistent, moisture.id())
            .has_value());
    CY_REQUIRE(registry.declare_consumer("vfx.smoke", SimulationClass::Presentation, wind.id())
                   .has_value());
    CY_REQUIRE(registry.declare_consumer("gameplay.boat", SimulationClass::Authoritative, wind.id())
                   .has_value());

    cy::Array<cy::environment::FirewallViolation> violations(test::allocator());
    CY_REQUIRE(registry.validate(violations).has_value());
    CY_REQUIRE_EQ(violations.size(), 1u);
    CY_CHECK(test::same_text(violations[0].consumer, "gameplay.boat"));
    CY_CHECK(test::same_text(violations[0].field_name, "test.wind"));
    CY_CHECK(violations[0].reader_class == SimulationClass::Authoritative);
    CY_CHECK(violations[0].field_class == SimulationClass::Presentation);
}

CY_TEST_CASE("every reader of one field observes one value, whatever class it reads as") {
    // "WHEN terrain material, foliage placement, and audio all need wetness at a point, THEN they
    // SHALL sample the same field and observe the same value." Which sampling path a reader gets is
    // decided by the FIELD, so a gameplay-visible field reads deterministically for the renderer
    // too — dispatching on the reader's class would make this scenario false for exactly the fields
    // where it matters.
    FieldRegistry registry(test::allocator());
    const cy::world::PartitionConfig partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);
    const FieldDeclaration moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());
    cy::Expected<ProducerToken, cy::Error> token =
        registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(token.has_value());
    CY_REQUIRE(test::fill_tile(store, *token, test::tile_at(moisture.id(), 2, 0, 0),
                               FieldValue::scalar(0.5F))
                   .has_value());
    CY_REQUIRE(test::fill_tile(store, *token, test::tile_at(moisture.id(), 0, 0, 0),
                               FieldValue::scalar(0.9F))
                   .has_value());

    cy::Expected<FieldReader, cy::Error> gameplay =
        FieldReader::open(store, moisture.id(), SimulationClass::Persistent);
    cy::Expected<FieldReader, cy::Error> audio =
        FieldReader::open(store, moisture.id(), SimulationClass::Presentation);
    CY_REQUIRE(gameplay.has_value());
    CY_REQUIRE(audio.has_value());
    CY_CHECK(gameplay->deterministic());
    CY_CHECK(audio->deterministic());

    const cy::world::WorldVec3d where = test::at(10.0, 10.0);
    CY_CHECK_EQ(gameplay->sample(where).value.x(), audio->sample(where).value.x());
    CY_CHECK_NEAR(gameplay->sample(where).value.x(), 0.5F, 1.0F / 255.0F);

    // A presentation-classified field takes the other path, for every reader of it.
    const FieldDeclaration wind = test::wind_like();
    CY_REQUIRE(registry.declare(wind).has_value());
    cy::Expected<FieldReader, cy::Error> visual =
        FieldReader::open(store, wind.id(), SimulationClass::Presentation);
    CY_REQUIRE(visual.has_value());
    CY_CHECK_FALSE(visual->deterministic());
}
