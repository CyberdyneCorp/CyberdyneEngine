// M8.b task 3.1 and 3.2 — gameplay tags, teams and relationships, ownership and authority, and
// session state fragments.
//
// Every case below is one of `gameplay-framework`'s own scenarios, and the two that are not are
// there because a scenario would have passed against an implementation that is wrong: the
// `Unit.RobotFactory` case, which a string-prefix tag match fails, and the "parts inherit" case,
// which reads a part's STORED owner rather than only its resolved one.

// ================================================================================================
// A GAME WITHOUT ABILITIES PAYS NOTHING, ASSERTED THE WAY test_bypass.cpp ASSERTS ITS INVARIANT
// ================================================================================================
//
// `gameplay-abilities-and-effects`, first requirement: "A project that does not use abilities SHALL
// link **none** of this capability's code and SHALL carry **none** of its data."
//
// `cy::gameplay` does not depend on `cy::gameplay-abilities`, so this suite's include path cannot
// see an ability header. If somebody adds the dependency — to share a type, to reuse the attribute
// store, for any of the reasons that sound sensible one at a time — this becomes reachable and the
// assertion below fails with the reason. `abilities/` depending on `gameplay/` is the direction
// that keeps the requirement true; the reverse is the one that quietly ends it.
#if __has_include(<cy/gameplay/abilities/abilities.h>)
static_assert(false,
              "src/gameplay/ can see an ability header. That means cy::gameplay-abilities was "
              "added to this module's dependencies, and `gameplay-abilities-and-effects` says a "
              "project that does not use abilities links none of that code. The dependency points "
              "the other way: abilities builds ON the framework. Remove it; do not delete this "
              "check.");
#endif

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/fragments.h>
#include <cy/gameplay/ownership.h>
#include <cy/gameplay/tags.h>
#include <cy/gameplay/teams.h>
#include <cy/test/test.h>

using namespace cy::gameplay;
using cy::Name;
using cy::u32;
using cy::u64;
using cy::ecs::Entity;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

[[nodiscard]] Entity entity(u32 index) noexcept {
    return Entity::make(index, 1);
}

}  // namespace

CY_TEST_CASE("gameplay_tags: a query for Unit.Robot matches Unit.Robot.Harvester") {
    TagRegistry registry(allocator());
    auto harvester = registry.declare("Unit.Robot.Harvester");
    CY_REQUIRE(harvester.has_value());
    // Declaring the leaf declared its ancestors: a hierarchy with a hole cannot answer.
    const TagId robot = registry.find("Unit.Robot");
    const TagId unit = registry.find("Unit");
    CY_REQUIRE_NE(robot, kInvalidTag);
    CY_REQUIRE_NE(unit, kInvalidTag);

    CY_CHECK(registry.matches(robot, *harvester));
    CY_CHECK(registry.matches(unit, *harvester));
    CY_CHECK(registry.matches(*harvester, *harvester));
    // And not the other way: a parent is not a descendant of its child.
    CY_CHECK_FALSE(registry.matches(*harvester, robot));
}

CY_TEST_CASE("gameplay_tags: Unit.RobotFactory does not match Unit.Robot") {
    // THE CASE A STRING PREFIX GETS WRONG. "Unit.Robot" IS a text prefix of "Unit.RobotFactory",
    // and the tag is not an ancestor of it. `gameplay-framework` names the mechanism — "compact
    // metadata, not string prefix comparison" — precisely because the outcome differs here.
    TagRegistry registry(allocator());
    auto robot = registry.declare("Unit.Robot");
    auto factory = registry.declare("Unit.RobotFactory");
    CY_REQUIRE(robot.has_value());
    CY_REQUIRE(factory.has_value());
    CY_CHECK_FALSE(registry.matches(*robot, *factory));
    CY_CHECK(registry.matches(registry.find("Unit"), *factory));
}

CY_TEST_CASE("gameplay_tags: a tag test is an integer operation over a set") {
    TagRegistry registry(allocator());
    auto stunned = registry.declare("State.Stunned");
    auto burning = registry.declare("State.Burning");
    auto damage = registry.declare("Damage.Fire");
    CY_REQUIRE(stunned.has_value());
    CY_REQUIRE(burning.has_value());
    CY_REQUIRE(damage.has_value());

    TagSet set(allocator());
    CY_REQUIRE(set.add(*burning).has_value());
    CY_REQUIRE(set.add(*damage).has_value());
    // Idempotent, and sorted: membership is a binary search over `u32`s.
    CY_REQUIRE(set.add(*burning).has_value());
    CY_CHECK_EQ(set.size(), 2U);
    CY_CHECK(set.has_exact(*burning));
    CY_CHECK_FALSE(set.has_exact(*stunned));
    CY_CHECK(set.has(registry, registry.find("State")));
    CY_CHECK_FALSE(set.has(registry, *stunned));
    CY_CHECK(set.remove(*burning));
    CY_CHECK_FALSE(set.has(registry, registry.find("State")));
}

CY_TEST_CASE("gameplay_tags: tooling reports a tag doing an archetype's job") {
    // `gameplay-framework`'s "The distinction is enforced": a frequently queried classification
    // expressed as a gameplay tag SHALL be reportable as a performance issue.
    TagRegistry registry(allocator());
    auto building = registry.declare("Unit.Building");
    auto stunned = registry.declare("State.Stunned");
    CY_REQUIRE(building.has_value());
    CY_REQUIRE(stunned.has_value());

    EntityTagStore store(allocator());
    for (u32 index = 1; index <= 40; ++index) {
        CY_REQUIRE(store.add(entity(index), *building).has_value());
    }
    CY_REQUIRE(store.add(entity(1), *stunned).has_value());
    for (u32 round = 0; round < 100; ++round) {
        (void)store.has(registry, entity(1), *building);
    }
    (void)store.has(registry, entity(1), *stunned);

    CY_CHECK_EQ(store.carriers(*building), 40U);
    CY_CHECK_EQ(store.queries(*building), 100U);
    EntityTagStore::ScanRisk risks[4] = {};
    const u32 found = store.scan_risk(32, 50, risks, 4);
    CY_REQUIRE_EQ(found, 1U);
    CY_CHECK_EQ(risks[0].tag, *building);
    // The rarely queried one is not reported: the tooling names the classification, not every tag.
    CY_CHECK_EQ(store.scan_risk(1000, 50, nullptr, 0), 0U);
}

CY_TEST_CASE("gameplay_teams: not everyone else is an enemy") {
    RelationshipService service(allocator());
    auto red = service.add_team(Name::intern("red"));
    auto blue = service.add_team(Name::intern("blue"));
    auto green = service.add_team(Name::intern("green"));
    CY_REQUIRE(red.has_value());
    CY_REQUIRE(blue.has_value());
    CY_REQUIRE(green.has_value());

    // Three teams, two allied. `a != b` would make all three pairs hostile.
    CY_REQUIRE(service.set_relationship(*red, *blue, Relationship::Ally).has_value());
    CY_REQUIRE(service.set_relationship(*red, *green, Relationship::Hostile).has_value());

    CY_CHECK_EQ(service.between(*red, *blue), Relationship::Ally);
    CY_CHECK_EQ(service.between(*blue, *red), Relationship::Ally);
    CY_CHECK_EQ(service.between(*red, *green), Relationship::Hostile);
    CY_CHECK_EQ(service.between(*red, *red), Relationship::Self);
    // Declared nowhere, and therefore NEUTRAL rather than hostile.
    CY_CHECK_EQ(service.between(*blue, *green), Relationship::Neutral);
}

CY_TEST_CASE("gameplay_teams: an alliance forms mid-match and every consumer sees it at once") {
    RelationshipService service(allocator());
    auto red = service.add_team(Name::intern("red"));
    auto blue = service.add_team(Name::intern("blue"));
    CY_REQUIRE(red.has_value());
    CY_REQUIRE(blue.has_value());
    CY_REQUIRE(service.set_relationship(*red, *blue, Relationship::Hostile).has_value());
    CY_REQUIRE(service.set_team(entity(7), *blue).has_value());

    Participant commander;
    commander.team = *red;
    CY_CHECK_EQ(service.between(commander, entity(7)), Relationship::Hostile);

    CY_REQUIRE(service.set_relationship(*red, *blue, Relationship::Ally).has_value());
    // ONE SERVICE. Targeting, the AI and the interface all read this call, so there is no
    // arrangement in which one of them still believes the truce has not happened.
    CY_CHECK_EQ(service.between(commander, entity(7)), Relationship::Ally);
    CY_CHECK_EQ(service.between(*red, *blue), Relationship::Ally);
}

CY_TEST_CASE("gameplay_teams: a unit belongs to a team, a faction and a squad at once") {
    RelationshipService service(allocator());
    auto team = service.add_team(Name::intern("red"));
    CY_REQUIRE(team.has_value());
    CY_REQUIRE(service.set_team(entity(1), *team).has_value());
    CY_REQUIRE(service.add_affiliation(entity(1), Name::intern("faction"), 12).has_value());
    CY_REQUIRE(service.add_affiliation(entity(1), Name::intern("squad"), 3).has_value());
    CY_REQUIRE(service.add_affiliation(entity(2), Name::intern("squad"), 3).has_value());

    Affiliation held[4] = {};
    CY_CHECK_EQ(service.affiliations_of(entity(1), held, 4), 2U);
    CY_CHECK_EQ(service.affiliation_of(entity(1), Name::intern("faction")).id, 12U);
    CY_CHECK(service.share_affiliation(entity(1), entity(2), Name::intern("squad")));
    CY_CHECK_FALSE(service.share_affiliation(entity(1), entity(2), Name::intern("faction")));
}

CY_TEST_CASE("gameplay_ownership: a captured turret is three things, correctly and separately") {
    // `gameplay-framework`'s own scenario. Ownership is the enemy faction's, control is the
    // player's, authority is the server's — and no one of the three is inferred from another.
    RelationshipService relationships(allocator());
    auto enemy = relationships.add_team(Name::intern("enemy"));
    CY_REQUIRE(enemy.has_value());

    OwnershipRegistry ownership(allocator());
    const Entity turret = entity(9);
    CY_REQUIRE(ownership.set_owner(turret, Owner::of_team(*enemy)).has_value());
    CY_REQUIRE(ownership.set_authority(turret, NetworkAuthority::Server).has_value());

    CY_CHECK_EQ(ownership.owner(turret).kind, OwnerKind::Team);
    CY_CHECK_EQ(ownership.owner(turret).team, *enemy);
    CY_CHECK_EQ(ownership.authority(turret), NetworkAuthority::Server);
    // Control is not here at all — it is `ControlRegistry`'s, and it is many-to-many and
    // channelled while these two are one value each. Merging them would make this turret a
    // special case.
    CY_CHECK(ownership.owner(turret).participant.is_null());
}

CY_TEST_CASE("gameplay_ownership: parts resolve the new owner without each storing a copy") {
    OwnershipRegistry ownership(allocator());
    const Entity robot = entity(1);
    const Entity arm = entity(2);
    const Entity finger = entity(3);
    const auto first = cy::gameplay::ParticipantId::from_slot(0, 1);
    const auto second = cy::gameplay::ParticipantId::from_slot(1, 1);

    CY_REQUIRE(ownership.set_owner(robot, Owner::of_participant(first)).has_value());
    CY_REQUIRE(ownership.set_owner(arm, Owner::inherited()).has_value());
    CY_REQUIRE(ownership.set_parent(arm, robot).has_value());
    CY_REQUIRE(ownership.set_owner(finger, Owner::inherited()).has_value());
    CY_REQUIRE(ownership.set_parent(finger, arm).has_value());

    CY_CHECK_EQ(ownership.resolve_owner(finger).participant, first);
    CY_REQUIRE(ownership.set_owner(robot, Owner::of_participant(second)).has_value());
    CY_CHECK_EQ(ownership.resolve_owner(finger).participant, second);
    CY_CHECK_EQ(ownership.resolve_owner(arm).participant, second);
    // THE ASSERTION THAT MATTERS: the part still stores `Inherited`. Nothing was written to it, so
    // there is no copy that could go stale.
    CY_CHECK_EQ(ownership.owner(finger).kind, OwnerKind::Inherited);
    CY_CHECK_EQ(ownership.owned_by(second, nullptr, 0), 3U);
    CY_CHECK_EQ(ownership.owned_by(first, nullptr, 0), 0U);
}

CY_TEST_CASE("gameplay_ownership: a mis-authored cycle resolves to unowned rather than hanging") {
    OwnershipRegistry ownership(allocator());
    CY_REQUIRE(ownership.set_owner(entity(1), Owner::inherited()).has_value());
    CY_REQUIRE(ownership.set_owner(entity(2), Owner::inherited()).has_value());
    CY_REQUIRE(ownership.set_parent(entity(1), entity(2)).has_value());
    CY_REQUIRE(ownership.set_parent(entity(2), entity(1)).has_value());
    CY_CHECK_EQ(ownership.resolve_owner(entity(1)).kind, OwnerKind::None);
}

CY_TEST_CASE("gameplay_fragments: one declaration, four consumers") {
    FragmentStore store(allocator());
    FragmentDeclaration clock;
    clock.name = Name::intern("match_clock");
    clock.stable_id = 1;
    clock.authority = FragmentAuthority::Server;
    clock.visibility = FragmentVisibility::Everyone;
    clock.persistence = PersistenceClass::SaveGame;
    clock.size = sizeof(u64);
    auto id = store.register_fragment(clock);
    CY_REQUIRE(id.has_value());

    // FOUR ANSWERS, ONE DECLARATION. None of them has a setter.
    CY_CHECK(store.replicated(*id));
    CY_CHECK(store.saved(*id));
    CY_CHECK(store.replay_relevant(*id));
    CY_CHECK(store.observable_by(*id, ObserverRelation::Other));

    FragmentDeclaration derived;
    derived.name = Name::intern("threat_map");
    derived.stable_id = 2;
    derived.persistence = PersistenceClass::Derived;
    derived.visibility = FragmentVisibility::AuthorityOnly;
    derived.size = sizeof(u32);
    auto second = store.register_fragment(derived);
    CY_REQUIRE(second.has_value());
    CY_CHECK_FALSE(store.replicated(*second));
    CY_CHECK_FALSE(store.saved(*second));
    CY_CHECK_FALSE(store.replay_relevant(*second));
    CY_CHECK_FALSE(store.observable_by(*second, ObserverRelation::Owner));
    CY_CHECK(store.observable_by(*second, ObserverRelation::Authority));

    u64 written = 8842;
    CY_REQUIRE(store.write_value(*id, written).has_value());
    u64 read = 0;
    CY_REQUIRE(store.read_value(*id, read).has_value());
    CY_CHECK_EQ(read, 8842U);
}

CY_TEST_CASE("gameplay_fragments: a feature registers state without modifying an engine type") {
    FragmentStore store(allocator());
    const auto player = cy::gameplay::ParticipantId::from_slot(0, 1);
    FragmentDeclaration score;
    score.name = Name::intern("score");
    score.stable_id = 10;
    score.scope = FragmentScope::Player;
    score.persistence = PersistenceClass::SessionTransient;
    score.size = sizeof(u32);
    auto id = store.register_fragment(score);
    CY_REQUIRE(id.has_value());
    CY_REQUIRE(store.add_player(player).has_value());

    // The feature arrives AFTER the participant did, and the participant's storage grows. A
    // downloadable mode activated mid-session is exactly this shape.
    FragmentDeclaration economy;
    economy.name = Name::intern("ore");
    economy.stable_id = 11;
    economy.scope = FragmentScope::Player;
    economy.persistence = PersistenceClass::SaveGame;
    economy.size = sizeof(u32);
    auto second = store.register_fragment(economy);
    CY_REQUIRE(second.has_value());

    u32 ore = 250;
    CY_REQUIRE(store.write_value(*second, ore, player).has_value());
    u32 read = 0;
    CY_REQUIRE(store.read_value(*second, read, player).has_value());
    CY_CHECK_EQ(read, 250U);
    // A player fragment addressed without a participant is refused rather than reading the
    // session's bytes.
    CY_CHECK_FALSE(store.read_value(*second, read).has_value());
    CY_CHECK_EQ(store.count(), 2U);
}
