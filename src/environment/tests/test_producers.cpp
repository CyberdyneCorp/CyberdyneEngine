// ONE PRODUCER PER FIELD, refused at registration, naming both producers. M10 tasks.md 1.2, whose
// exit criterion is the refusal itself.
//
// `environment-fields` — "One producer per field": "Two systems writing one field SHALL be a
// configuration error detected at startup or cook time, not a last-writer-wins race resolved at
// runtime", and its scenario: "WHEN two systems are configured to produce the same field, THEN the
// conflict SHALL be reported at configuration time and SHALL NOT be resolved by write order."
//
// HOW THIS SUITE WAS SHOWN TO BE ABLE TO FAIL, which the milestone's brief requires of the refusal
// specifically: the `record->claimed` guard in `FieldRegistry::claim()` was deleted, the suite was
// run, and "a second producer is refused, and the refusal names both of them" went red on its first
// assertion — `CY_REQUIRE_FALSE(second.has_value())` — with the second claim having succeeded and
// handed out a second writable token for one field. The guard was then restored. That sequence is
// reported in this milestone's `verified_failing`.

#include <cy/test/test.h>

#include <cy/environment/field.h>
#include <cy/environment/store.h>

#include <cstdio>

#include "fixtures.h"

using cy::environment::FieldRegistry;
using cy::environment::FieldStore;
using cy::environment::FieldValue;
using cy::environment::ProducerKind;
using cy::environment::ProducerToken;
namespace test = cy::environment::test;

namespace {

/// Does `text` contain `needle`? The refusal's message must NAME both producers, and a test that
/// asserted only on the error code would pass for a message that named neither.
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

CY_TEST_CASE("a second producer is refused, and the refusal names both of them") {
    FieldRegistry registry(test::allocator());
    const auto moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());

    cy::Expected<ProducerToken, cy::Error> first =
        registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(first.has_value());
    CY_CHECK(first->valid());

    // THE EXIT CRITERION. A second system configured to produce one field.
    cy::Expected<ProducerToken, cy::Error> second =
        registry.claim(moisture.id(), "weather.precipitation", ProducerKind::System);
    CY_REQUIRE_FALSE(second.has_value());
    CY_CHECK(second.error().code == cy::ErrorCode::AlreadyExists);

    // Both producers, in the error's own message — not only in a log line a caller may not read.
    CY_CHECK(contains(second.error().message, "terrain.hydrology"));
    CY_CHECK(contains(second.error().message, "weather.precipitation"));
    CY_CHECK(contains(second.error().message, "test.moisture"));

    // And structurally, so a diagnostic view does not have to parse a sentence.
    const cy::environment::ProducerConflict& conflict = registry.last_conflict();
    CY_CHECK(conflict.occurred());
    CY_CHECK(conflict.field == moisture.id());
    CY_CHECK(test::same_text(conflict.incumbent, "terrain.hydrology"));
    CY_CHECK(test::same_text(conflict.challenger, "weather.precipitation"));
    CY_CHECK(test::same_text(conflict.field_name, "test.moisture"));
    CY_CHECK_EQ(registry.conflict_count(), 1u);

    // "SHALL NOT be resolved by write order": the incumbent still holds the field after the
    // refusal, and the refusal changed nothing about who produces it.
    const cy::environment::FieldRecord* record = registry.find(moisture.id());
    CY_REQUIRE(record != nullptr);
    CY_CHECK(record->claimed);
    CY_CHECK(test::same_text(record->producer_name, "terrain.hydrology"));
}

CY_TEST_CASE("the refused producer cannot write, because it has no token to write with") {
    // The refusal is the diagnostic; the token is the guarantee. `FieldStore` accepts writes only
    // through a `ProducerToken`, which `claim()` issues once and which is move-only — so there is
    // no expression that gives a second system a writer, even one that ignored the error above.
    FieldRegistry registry(test::allocator());
    const auto moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());
    const auto partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);

    cy::Expected<ProducerToken, cy::Error> holder =
        registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(holder.has_value());
    CY_REQUIRE(
        registry.claim(moisture.id(), "weather.precipitation", ProducerKind::System).has_value() ==
        false);

    // What the refused system has is a default token, and it opens nothing.
    const ProducerToken empty;
    CY_CHECK_FALSE(empty.valid());
    CY_CHECK_FALSE(store.open_writer(empty).has_value());
    CY_CHECK_FALSE(
        store.insert_default_tile(empty, test::tile_at(moisture.id(), 2, 0, 0), false).has_value());

    // The incumbent writes normally.
    CY_REQUIRE(store.open_writer(*holder).has_value());
    CY_REQUIRE(test::fill_tile(store, *holder, test::tile_at(moisture.id(), 2, 0, 0),
                               FieldValue::scalar(0.5F))
                   .has_value());
    CY_CHECK(store.is_resident(test::tile_at(moisture.id(), 2, 0, 0)));

    // A token is for ONE field. Holding moisture's does not make a writer for biome.
    CY_REQUIRE(registry.declare(test::biome_like()).has_value());
    const auto biome = test::biome_like().id();
    CY_CHECK_FALSE(
        store.insert_default_tile(*holder, test::tile_at(biome, 2, 0, 0), false).has_value());
}

CY_TEST_CASE("a token moved to another holder leaves nothing behind") {
    // Move-only is what makes the second writer unspellable rather than merely refused: a system
    // that hands its token on no longer has one, so "exactly one producer AT ANY TIME" survives a
    // handover instead of being broken by it.
    FieldRegistry registry(test::allocator());
    const auto moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());

    cy::Expected<ProducerToken, cy::Error> claimed =
        registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(claimed.has_value());

    ProducerToken handed_over = std::move(*claimed);
    CY_CHECK(handed_over.valid());
    CY_CHECK_FALSE(claimed->valid());
    CY_CHECK(test::same_text(handed_over.producer_name(), "terrain.hydrology"));
}

CY_TEST_CASE("a claim on an undeclared field, and an unnamed producer, are both refused") {
    FieldRegistry registry(test::allocator());
    const auto moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());

    CY_CHECK_FALSE(
        registry
            .claim(cy::environment::field_id("nothing.declared"), "somebody", ProducerKind::System)
            .has_value());
    // A producer that will not name itself cannot be named in a conflict, which is the whole of the
    // refusal above.
    CY_CHECK_FALSE(registry.claim(moisture.id(), "", ProducerKind::System).has_value());
    CY_CHECK_EQ(registry.conflict_count(), 0u);

    // A field with no producer is legal — a cooked field nobody writes at run time is the ordinary
    // case — and reported rather than refused.
    cy::Array<cy::environment::FieldId> unclaimed(test::allocator());
    CY_REQUIRE(registry.unclaimed(unclaimed).has_value());
    CY_REQUIRE_EQ(unclaimed.size(), 1u);
    CY_CHECK(unclaimed[0] == moisture.id());
}

CY_TEST_CASE(
    "a baked source, a system and a generator are three kinds of one producer, not three "
    "producers") {
    // "a baked source, a system that writes it, or a project-supplied generator" — one of the
    // three, never two of them. The kind is recorded; the exclusivity is the same.
    FieldRegistry registry(test::allocator());
    const auto biome = test::biome_like();
    CY_REQUIRE(registry.declare(biome).has_value());

    CY_REQUIRE(registry.claim(biome.id(), "cook.biome-bake", ProducerKind::Baked).has_value());
    CY_CHECK_FALSE(
        registry.claim(biome.id(), "pcg.biome-generator", ProducerKind::Generator).has_value());
    CY_CHECK(test::same_text(registry.last_conflict().incumbent, "cook.biome-bake"));
    CY_CHECK(test::same_text(registry.last_conflict().challenger, "pcg.biome-generator"));

    const cy::environment::FieldRecord* record = registry.find(biome.id());
    CY_REQUIRE(record != nullptr);
    CY_CHECK(record->producer_kind == ProducerKind::Baked);
}

// ================================================================================================
// THE M10 GATE'S ADVERSARIAL PASS (tasks.md 9.3): TRYING TO GET TWO PRODUCERS ONTO ONE FIELD ANYWAY
// ================================================================================================
//
// The cases above hold the refusal that `claim()` makes. These try to get PAST it — a different
// module, a different order, a token that outlives its owner, the same field under a different
// residency — because a refusal is only worth what the paths around it are worth.

CY_TEST_CASE("a token minted by a SECOND registry does not write this store's field") {
    // THE ATTACK. `claim()`'s refusal is per REGISTRY, and a `FieldStore` holds exactly one. A
    // module that builds a registry of its own — an offline cooker, an editor validating a
    // declaration before the runtime exists, a test — can claim a field there that another module
    // already produces here, because that registry has never heard of the incumbent. The token it
    // gets back is a plain value: a field identity, a producer identity and a name.
    //
    // So the store is the second place the rule has to hold, and it holds it by asking ITS OWN
    // registry who produces the field rather than by trusting the token it was handed.
    FieldRegistry engine(test::allocator());
    const auto moisture = test::moisture_like();
    CY_REQUIRE(engine.declare(moisture).has_value());
    const auto partition = test::partition();
    FieldStore store(test::allocator(), engine, partition);

    cy::Expected<ProducerToken, cy::Error> incumbent =
        engine.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(incumbent.has_value());
    const auto tile = test::tile_at(moisture.id(), 2, 0, 0);
    CY_REQUIRE(test::fill_tile(store, *incumbent, tile, FieldValue::scalar(0.25F)).has_value());

    // A second registry, declaring the same field IDENTICALLY — which `declare()` accepts on
    // purpose, so that two modules needing `wetness` need not agree on which of them declares it.
    FieldRegistry other(test::allocator());
    CY_REQUIRE(other.declare(moisture).has_value());
    cy::Expected<ProducerToken, cy::Error> interloper =
        other.claim(moisture.id(), "weather.precipitation", ProducerKind::System);
    CY_REQUIRE(interloper.has_value());
    CY_CHECK_EQ(engine.conflict_count(), 0u);  // the engine's registry was never asked

    // The attack itself: a writable view of a field this producer does not produce.
    CY_CHECK_FALSE(store.open_writer(*interloper).has_value());
    CY_CHECK_FALSE(
        test::fill_tile(store, *interloper, tile, FieldValue::scalar(0.99F)).has_value());
    CY_CHECK_FALSE(
        store.insert_default_tile(*interloper, test::tile_at(moisture.id(), 2, 1, 0), false)
            .has_value());
    CY_CHECK_FALSE(store.evict_tile(*interloper, tile).has_value());
    CY_CHECK_FALSE(store.advance_recovery(*interloper, 1.0F).has_value());

    // And the incumbent's value is what a reader still sees: the refusal did not resolve by write
    // order, which is the requirement's own sentence.
    const cy::environment::FieldSample sample = store.sample(moisture.id(), test::at(1.0, 1.0));
    CY_CHECK(sample.resolved);
    CY_CHECK_LT(sample.value.x(), 0.5F);

    // The incumbent is unaffected by the attempt.
    CY_CHECK(store.open_writer(*incumbent).has_value());
}

CY_TEST_CASE("a producer token that outlives its holder still names one producer, and only one") {
    // A token is a value with no destructor and no release: a module that claims a field and then
    // goes away leaves the field CLAIMED, so the next module to ask is refused rather than handed
    // a second writer. That is the conservative direction on purpose — the alternative is a field
    // whose producer changes when an unrelated object is destroyed.
    FieldRegistry registry(test::allocator());
    const auto moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());

    {
        cy::Expected<ProducerToken, cy::Error> transient =
            registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
        CY_REQUIRE(transient.has_value());
    }  // the holder is gone, and the claim is not

    CY_CHECK_FALSE(
        registry.claim(moisture.id(), "weather.precipitation", ProducerKind::System).has_value());
    CY_CHECK(test::same_text(registry.last_conflict().incumbent, "terrain.hydrology"));
}

CY_TEST_CASE("one producer per FIELD, not one per residency level or per layer") {
    // THE ATTACK. A field has three residency levels and two layers, and a second system that
    // cannot have the field might still hope to have a corner of it — the Delta layer, or the
    // Local level nothing else writes. `claim()` is per field identity, so there is no corner to
    // take, and the store's addresses carry the field identity the token is checked against.
    FieldRegistry registry(test::allocator());
    const auto moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());
    const auto partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);

    cy::Expected<ProducerToken, cy::Error> incumbent =
        registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(incumbent.has_value());

    const char* per_level[3] = {"weather.local", "weather.regional", "weather.macro"};
    for (const char* challenger : per_level) {
        CY_CHECK_FALSE(registry.claim(moisture.id(), challenger, ProducerKind::System).has_value());
        CY_CHECK(test::same_text(registry.last_conflict().challenger, challenger));
    }
    CY_CHECK_EQ(registry.conflict_count(), 3u);

    // The incumbent writes every level and both layers through the one token it holds.
    CY_REQUIRE(test::fill_tile(store, *incumbent, test::tile_at(moisture.id(), 2, 0, 0),
                               FieldValue::scalar(0.25F))
                   .has_value());
    CY_REQUIRE(
        test::fill_tile(store, *incumbent,
                        test::tile_at(moisture.id(), 1, 0, 0, cy::environment::FieldLayer::Delta),
                        FieldValue::scalar(0.75F))
            .has_value());
}

CY_TEST_CASE("a writer keeps reading the right declaration after the registry grows") {
    // THE ATTACK. `open_writer()` hands the writer the declaration the registry holds, and the
    // registry holds its records in a growable array. A producer that opened a writer and then let
    // any other module declare a field would be writing through a declaration the array had moved
    // out from under it — an encoding and a range read from freed memory, which decides what every
    // byte this writer stores MEANS.
    FieldRegistry registry(test::allocator());
    const auto moisture = test::moisture_like();
    CY_REQUIRE(registry.declare(moisture).has_value());
    const auto partition = test::partition();
    FieldStore store(test::allocator(), registry, partition);

    cy::Expected<ProducerToken, cy::Error> token =
        registry.claim(moisture.id(), "terrain.hydrology", ProducerKind::System);
    CY_REQUIRE(token.has_value());
    cy::Expected<cy::environment::FieldWriter, cy::Error> writer = store.open_writer(*token);
    CY_REQUIRE(writer.has_value());

    // Anything else in the engine declaring a field. Enough of them that the array must reallocate.
    CY_REQUIRE(registry.declare(test::biome_like()).has_value());
    CY_REQUIRE(registry.declare(test::wind_like()).has_value());
    CY_REQUIRE(registry.declare(test::project_radiation()).has_value());
    for (cy::u32 index = 0; index < 64; ++index) {
        auto extra = test::moisture_like();
        static char names[64][32];
        (void)std::snprintf(names[index], sizeof(names[index]), "test.filler%u", index);
        extra.name = names[index];
        CY_REQUIRE(registry.declare(extra).has_value());
    }

    // The writer still writes moisture, still quantised over moisture's declared range.
    const auto tile = test::tile_at(moisture.id(), 2, 0, 0);
    CY_REQUIRE(writer->stage(tile).has_value());
    CY_REQUIRE(writer->fill(tile, FieldValue::scalar(0.5F)).has_value());
    CY_REQUIRE(writer->publish().has_value());
    const cy::environment::FieldSample sample = store.sample(moisture.id(), test::at(1.0, 1.0));
    CY_REQUIRE(sample.resolved);
    CY_CHECK_NEAR(sample.value.x(), 0.5F, 0.01F);
}
