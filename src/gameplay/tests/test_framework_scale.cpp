// M8.b task 3.3 — the two framework properties that are only meaningful at scale, and are
// therefore in `integration` rather than `unit`.
//
// Both build large structures per case. The `unit` tier's budget is a millisecond of CPU and a case
// that fills it by trimming its own population would be measuring nothing, so these live here — the
// same remedy `src/gameplay/play/tests/CMakeLists.txt` records for batch spawning.

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/events.h>
#include <cy/gameplay/features.h>
#include <cy/gameplay/fragments.h>
#include <cy/gameplay/indexes.h>
#include <cy/gameplay/interaction.h>
#include <cy/gameplay/ownership.h>
#include <cy/gameplay/rules.h>
#include <cy/gameplay/time.h>
#include <cy/test/test.h>

using namespace cy::gameplay;
using cy::Name;
using cy::u32;
using cy::u64;
using cy::Vec3;
using cy::ecs::Entity;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

[[nodiscard]] Entity entity(u32 index) noexcept {
    return Entity::make(index, 1);
}

}  // namespace

CY_TEST_CASE("gameplay_scale: fifty thousand pending timers cost the ones actually due") {
    // `gameplay-framework`'s "Many timers are cheap": "WHEN fifty thousand timers are pending THEN
    // advancing a tick SHALL cost work proportional to the timers actually due."
    TimerWheel wheel(allocator());
    for (u32 index = 0; index < 50000; ++index) {
        // Spread over a long horizon, deliberately: a fixed ring of N buckets would put fifty
        // thousand over N of these into whichever bucket a tick lands on and examine all of them.
        const u64 due = 100 + (static_cast<u64>(index) * 7);
        CY_REQUIRE(wheel.schedule(due, index).has_value());
    }
    CY_CHECK_EQ(wheel.pending(), 50000U);

    TimerExpiry fired[8] = {};
    // A tick nothing is due at examines NOTHING.
    CY_CHECK_EQ(wheel.advance_to(99, fired, 8), 0U);
    CY_CHECK_EQ(wheel.last_examined(), 0U);
    for (u64 tick = 100; tick < 106; ++tick) {
        const u32 count = wheel.advance_to(tick, fired, 8);
        CY_CHECK_EQ(wheel.last_examined(), count);
        CY_CHECK_LE(count, 1U);
    }
    // And the one tick that IS due examines exactly the one timer due at it.
    CY_CHECK_EQ(wheel.advance_to(107, fired, 8), 1U);
    CY_CHECK_EQ(wheel.last_examined(), 1U);
    CY_CHECK_EQ(wheel.pending(), 49998U);
}

CY_TEST_CASE(
    "gameplay_scale: a hundred thousand entities and an ownership query that is not a scan") {
    // "WHEN a player's units are enumerated in a hundred-thousand-entity world THEN the query SHALL
    // use an index rather than a full scan."
    GameplayIndexes indexes(allocator());
    const auto player = ParticipantId::from_slot(0, 1);
    const auto other = ParticipantId::from_slot(1, 1);
    for (u32 index = 1; index <= 100000; ++index) {
        const auto owner = (index % 1000) == 0 ? player : other;
        CY_REQUIRE(indexes.on_owner_changed(Entity::make(index, 1), owner).has_value());
    }
    CY_CHECK_EQ(indexes.tracked_entities(), 100000U);

    indexes.reset_counters();
    Entity found[128] = {};
    const u32 count = indexes.owned_by(player, found, 128);
    CY_CHECK_EQ(count, 100U);
    // A HUNDRED, NOT A HUNDRED THOUSAND. The number is the requirement; a comment saying "indexed"
    // would survive the change that stops indexing it.
    CY_CHECK_EQ(indexes.entities_scanned(), 100U);
}

CY_TEST_CASE("gameplay_interaction: a bulk query is spatial, not a scan") {
    TagRegistry tags(allocator());
    EntityTagStore tag_store(allocator());
    InteractionRegistry interactions(allocator(), 4.0F);

    // A thousand interactables spread over a wide grid, and one query with a small radius.
    InteractionOption option;
    option.action = kInvalidTag;
    option.range = 2.0F;
    option.command = 1;
    for (u32 index = 0; index < 1000; ++index) {
        const u32 column = index % 50;
        const u32 row = index / 50;
        const auto x = static_cast<cy::f32>(column * 8);
        const auto z = static_cast<cy::f32>(row * 8);
        CY_REQUIRE(interactions.add_interactable(entity(index + 1), Vec3{x, 0.0F, z}).has_value());
        CY_REQUIRE(interactions.add_option(entity(index + 1), option).has_value());
    }
    CY_CHECK_EQ(interactions.interactable_count(), 1000U);

    cy::Array<InteractionCandidate> results(allocator());
    InteractionQuery query;
    query.interactor = entity(9999);
    query.origin = Vec3{0.0F, 0.0F, 0.0F};
    query.radius = 2.0F;
    InteractionQueryReport report;
    CY_REQUIRE(
        interactions
            .query_batch(cy::Span<InteractionQuery>(&query, 1), tag_store, tags, results, report)
            .has_value());

    CY_CHECK_EQ(query.result_count, 1U);
    // THE MEASUREMENT. A scan would have tested a thousand; the grid tested a handful. Without
    // this number, the refactor that turns the grid back into a loop is invisible.
    CY_CHECK_LT(report.candidates_tested, 20U);
    CY_CHECK_LE(report.cells_visited, 8U);
}

CY_TEST_CASE("gameplay_scale: the framework is destroyed full, in the wrong order, twice") {
    // HARD RULE: teardown is tested under load. Every structure below owns nested containers — a
    // bucket holds an array and a hash table, a tag store holds a set per entity, a feature holds
    // two arrays — and a destructor that walked one of those after its allocator had gone would
    // never be reached by a suite that only tears down empty ones.
    //
    // Twice, so that the second round runs against an allocator that has already served and
    // released the first, and everything is destroyed in reverse declaration order while full.
    for (u32 round = 0; round < 2; ++round) {
        TagRegistry tags(allocator());
        EntityTagStore tag_store(allocator());
        RelationshipService relationships(allocator());
        OwnershipRegistry ownership(allocator());
        GameplayIndexes indexes(allocator());
        InteractionRegistry interactions(allocator());
        FragmentStore fragments(allocator());
        FeatureRegistry features(allocator());
        PhaseController phases(allocator());
        TimeDomains domains(allocator());
        TimerService timers(allocator(), domains);
        EventBus events(allocator());

        auto robot = tags.declare("Unit.Robot");
        auto team = relationships.add_team(Name::intern("red"));
        CY_REQUIRE(robot.has_value());
        CY_REQUIRE(team.has_value());
        const auto owner = ParticipantId::from_slot(0, 1);

        InteractionOption option;
        option.action = *robot;
        option.range = 2.0F;
        option.command = 1;
        constexpr u32 kEntities = 20000;
        for (u32 index = 1; index <= kEntities; ++index) {
            const Entity subject = Entity::make(index, 1);
            CY_REQUIRE(tag_store.add(subject, *robot).has_value());
            CY_REQUIRE(relationships.set_team(subject, *team).has_value());
            CY_REQUIRE(relationships.add_affiliation(subject, Name::intern("squad"), index % 32)
                           .has_value());
            CY_REQUIRE(ownership.set_owner(subject, Owner::of_participant(owner)).has_value());
            CY_REQUIRE(indexes.on_owner_changed(subject, owner).has_value());
            CY_REQUIRE(indexes.on_tag_added(subject, *robot).has_value());
            const auto x = static_cast<cy::f32>(index % 128) * 3.0F;
            CY_REQUIRE(interactions.add_interactable(subject, Vec3{x, 0.0F, 0.0F}).has_value());
            CY_REQUIRE(interactions.add_option(subject, option).has_value());
        }
        const DomainId gameplay = domains.builtin(TimeDomainKind::Gameplay);
        CY_REQUIRE(timers.add_domain(gameplay).has_value());
        for (u32 index = 0; index < 20000; ++index) {
            CY_REQUIRE(timers.wheel(gameplay)->schedule(1000 + index, index).has_value());
        }
        EventDeclaration noise;
        noise.name = Name::intern("Noise");
        noise.stable_id = 1;
        auto type = events.declare(noise);
        CY_REQUIRE(type.has_value());
        auto producer = events.open_producer(Name::intern("stress"));
        CY_REQUIRE(producer.has_value());
        for (u32 index = 0; index < 5000; ++index) {
            GameplayEvent event;
            event.type = *type;
            event.tick = index;
            CY_REQUIRE(events.publish(*producer, event).has_value());
        }
        for (u32 index = 0; index < 32; ++index) {
            char name[16] = {};
            name[0] = 'f';
            name[1] = static_cast<char>('a' + static_cast<int>(index % 26));
            name[2] = static_cast<char>('0' + static_cast<int>(index / 26));
            auto feature = features.register_feature(Name::intern(name));
            CY_REQUIRE(feature.has_value());
            CY_REQUIRE(
                features.contribute(*feature, ContributionKind::System, Name::intern("System"))
                    .has_value());
        }

        CY_CHECK_EQ(indexes.tracked_entities(), kEntities);
        CY_CHECK_EQ(interactions.interactable_count(), kEntities);
        CY_CHECK_EQ(events.pending_count(), 5000U);

        // Half dismantled explicitly, so the destructors run over a structure with holes in it.
        for (u32 index = 1; index <= kEntities / 2; ++index) {
            const Entity subject = Entity::make(index, 1);
            indexes.on_entity_removed(subject);
            interactions.remove_interactable(subject);
            tag_store.forget(subject);
            relationships.forget(subject);
            ownership.forget(subject);
        }
        CY_CHECK_EQ(indexes.tracked_entities(), kEntities / 2);
    }
    CY_CHECK(true);
}
