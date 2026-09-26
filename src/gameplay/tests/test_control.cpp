// TASK 4.4.2 — control sources, channels, bindings and groups.
//
// The three scenarios `gameplay-framework` names are three different shapes and one model has to
// carry all of them: one source to many entities, many sources to one entity, and an AI and a human
// on one entity at once. A possession model carries none. The cases below are those three, plus the
// one that makes the first of them *cheap* — two hundred units is one binding, not two hundred.

#include "fixture.h"

using namespace cy::gameplay_test;
using cy::u32;

namespace {

struct ControlFixture : Fixture {
    [[nodiscard]] bool build() noexcept {
        auto added =
            session.add_participant(ParticipantKind::LocalHuman, cy::Name::intern("commander"));
        if (!added) {
            return false;
        }
        participant = *added;
        return true;
    }

    ParticipantId participant;
};

}  // namespace

CY_TEST_CASE("gameplay: a player commands an army through one binding") {
    ControlFixture fixture;
    CY_REQUIRE(fixture.build());
    auto source = fixture.control.create_source(ControlSourceKind::Human, fixture.participant,
                                                cy::Name::intern("player"));
    CY_REQUIRE(source.has_value());
    auto army = fixture.control.create_group(cy::Name::intern("selection"));
    CY_REQUIRE(army.has_value());
    for (u32 index = 0; index < 200; ++index) {
        CY_REQUIRE(fixture.control.add_to_group(*army, entity(index)).has_value());
    }
    CY_REQUIRE(fixture.control.bind_group(*source, channels::command(), *army).has_value());

    // TWO HUNDRED UNITS, ONE BINDING. `gameplay-framework`'s forbidden-patterns list names
    // "Representing a large controlled group as one control relationship per entity" — and this
    // assertion is what stops that reappearing as an optimisation somebody undoes.
    CY_CHECK_EQ(fixture.control.binding_count(), 1);
    CY_CHECK_EQ(fixture.control.group_size(*army), 200);

    // The relationship is nevertheless true of every member.
    CY_CHECK(fixture.control.controls(*source, entity(0), channels::command()));
    CY_CHECK(fixture.control.controls(*source, entity(199), channels::command()));
    CY_CHECK_FALSE(fixture.control.controls(*source, entity(200), channels::command()));
    CY_CHECK_EQ(fixture.control.controlled_entities(*source, channels::command(), nullptr, 0), 200);
}

CY_TEST_CASE("gameplay: two players share a vehicle on different channels") {
    ControlFixture fixture;
    CY_REQUIRE(fixture.build());
    auto second = fixture.session.add_participant(ParticipantKind::LocalHuman,
                                                  cy::Name::intern("gunner"), 0, 1);
    CY_REQUIRE(second.has_value());
    auto driver = fixture.control.create_source(ControlSourceKind::Human, fixture.participant,
                                                cy::Name::intern("driver"));
    auto gunner = fixture.control.create_source(ControlSourceKind::Human, *second,
                                                cy::Name::intern("gunner"));
    CY_REQUIRE(driver.has_value());
    CY_REQUIRE(gunner.has_value());

    const Entity tank = entity(7);
    CY_REQUIRE(fixture.control.bind_entity(*driver, channels::movement(), tank).has_value());
    CY_REQUIRE(fixture.control.bind_entity(*gunner, channels::turret(), tank).has_value());

    CY_CHECK(fixture.control.controls(*driver, tank, channels::movement()));
    CY_CHECK(fixture.control.controls(*gunner, tank, channels::turret()));
    // And neither can do the other's job. The channel is what makes that expressible without a
    // per-entity permission table.
    CY_CHECK_FALSE(fixture.control.controls(*driver, tank, channels::turret()));
    CY_CHECK_FALSE(fixture.control.controls(*gunner, tank, channels::movement()));

    ControlSourceId holders[4];
    CY_CHECK_EQ(fixture.control.sources_controlling(tank, holders, 4), 2);
}

CY_TEST_CASE("gameplay: an AI assists a human on the same entity") {
    ControlFixture fixture;
    CY_REQUIRE(fixture.build());
    auto human = fixture.control.create_source(ControlSourceKind::Human, fixture.participant,
                                               cy::Name::intern("player"));
    auto assist =
        fixture.control.create_source(ControlSourceKind::ArtificialIntelligence,
                                      fixture.participant, cy::Name::intern("aim-assist"));
    CY_REQUIRE(human.has_value());
    CY_REQUIRE(assist.has_value());

    const Entity avatar = entity(3);
    CY_REQUIRE(fixture.control.bind_entity(*human, channels::movement(), avatar).has_value());
    CY_REQUIRE(fixture.control.bind_entity(*assist, channels::weapons(), avatar).has_value());

    // Both hold bindings on one entity, on different channels, and the two sources belong to the
    // *same participant* — which is why a source is not a participant. See control.h.
    CY_CHECK(fixture.control.controls(*human, avatar, channels::movement()));
    CY_CHECK(fixture.control.controls(*assist, avatar, channels::weapons()));
    CY_CHECK_EQ(fixture.control.source(*assist)->participant.bits(), fixture.participant.bits());
    CY_CHECK_EQ(fixture.control.source(*assist)->kind, ControlSourceKind::ArtificialIntelligence);
}

CY_TEST_CASE("gameplay: a project channel needs no engine change") {
    ControlFixture fixture;
    CY_REQUIRE(fixture.build());
    auto source = fixture.control.create_source(ControlSourceKind::Script, fixture.participant,
                                                cy::Name::intern("cinematic"));
    CY_REQUIRE(source.has_value());
    // A `Name`, interned by the project. There is no enumeration to extend, which is what "or a
    // project-defined channel" requires.
    const cy::Name doors = cy::Name::intern("doors");
    CY_REQUIRE(fixture.control.bind_entity(*source, doors, entity(1)).has_value());
    CY_CHECK(fixture.control.controls(*source, entity(1), doors));
    CY_CHECK_FALSE(fixture.control.controls(*source, entity(1), channels::primary()));
}

CY_TEST_CASE("gameplay: unbinding a channel leaves the others alone") {
    ControlFixture fixture;
    CY_REQUIRE(fixture.build());
    auto source = fixture.control.create_source(ControlSourceKind::Human, fixture.participant,
                                                cy::Name::intern("player"));
    CY_REQUIRE(source.has_value());
    const Entity avatar = entity(2);
    CY_REQUIRE(fixture.control.bind_entity(*source, channels::movement(), avatar).has_value());
    CY_REQUIRE(fixture.control.bind_entity(*source, channels::camera(), avatar).has_value());

    fixture.control.unbind(*source, channels::camera());
    CY_CHECK(fixture.control.controls(*source, avatar, channels::movement()));
    CY_CHECK_FALSE(fixture.control.controls(*source, avatar, channels::camera()));
}

CY_TEST_CASE("gameplay: destroying a source takes its bindings with it") {
    ControlFixture fixture;
    CY_REQUIRE(fixture.build());
    auto source = fixture.control.create_source(ControlSourceKind::Human, fixture.participant,
                                                cy::Name::intern("player"));
    CY_REQUIRE(source.has_value());
    CY_REQUIRE(fixture.control.bind_entity(*source, channels::movement(), entity(1)).has_value());
    CY_REQUIRE(fixture.control.bind_entity(*source, channels::weapons(), entity(1)).has_value());
    CY_REQUIRE_EQ(fixture.control.binding_count(), 2);

    fixture.control.destroy_source(*source);
    // A binding whose source is gone would validate against a source that does not exist — which is
    // a rejection with the wrong reason, and a rule debugger pointing at nothing.
    CY_CHECK_EQ(fixture.control.binding_count(), 0);
    CY_CHECK(fixture.control.source(*source) == nullptr);
}

CY_TEST_CASE("gameplay: group membership changes without touching the binding") {
    ControlFixture fixture;
    CY_REQUIRE(fixture.build());
    auto source = fixture.control.create_source(ControlSourceKind::Human, fixture.participant,
                                                cy::Name::intern("player"));
    auto group = fixture.control.create_group(cy::Name::intern("selection"));
    CY_REQUIRE(source.has_value());
    CY_REQUIRE(group.has_value());
    CY_REQUIRE(fixture.control.bind_group(*source, channels::command(), *group).has_value());

    CY_REQUIRE(fixture.control.add_to_group(*group, entity(1)).has_value());
    CY_REQUIRE(fixture.control.add_to_group(*group, entity(2)).has_value());
    // Adding the same entity twice is a no-op rather than a duplicate: a selection that listed one
    // unit twice would send it two copies of every order.
    CY_REQUIRE(fixture.control.add_to_group(*group, entity(2)).has_value());
    CY_CHECK_EQ(fixture.control.group_size(*group), 2);

    fixture.control.remove_from_group(*group, entity(1));
    CY_CHECK_FALSE(fixture.control.controls(*source, entity(1), channels::command()));
    CY_CHECK(fixture.control.controls(*source, entity(2), channels::command()));
    // Still one binding. Changing a selection is not a change of control structure.
    CY_CHECK_EQ(fixture.control.binding_count(), 1);
}

// ==================================================================================================
// M11.d task 7.2 — THE STRATEGY STRESS SCENARIO'S FINDING, held at the registry's own level.
//
// `testing-and-quality`'s strategy stress is 5 000 agent groups. The registry refused a 65th group,
// and `controls()` answered by walking every binding and every group's member list — 94 % of a
// strategy tick at the scenario's scale. `controls()` now answers from two indexes, and these cases
// hold what the indexes must never get wrong: every way a binding or a membership goes AWAY must
// take the answer with it, and an entity in more groups than the inline capacity must still be
// answered exactly. The registry HOLDING five thousand groups is `integration.gameplay_scale`'s
// case, not this suite's: building five thousand groups sat at the unit kind's 1 ms CPU budget with
// no headroom and failed M11.d's full ledger at 1.017 ms (test_framework_scale.cpp says more).
// ==================================================================================================

CY_TEST_CASE("gameplay: the control index forgets exactly what is unbound or removed") {
    ControlFixture fixture;
    CY_REQUIRE(fixture.build());
    auto source = fixture.control.create_source(ControlSourceKind::Human, fixture.participant,
                                                cy::Name::intern("player"));
    auto other = fixture.control.create_source(ControlSourceKind::Human, fixture.participant,
                                               cy::Name::intern("other"));
    auto group = fixture.control.create_group(cy::Name::intern("selection"));
    CY_REQUIRE(source.has_value());
    CY_REQUIRE(other.has_value());
    CY_REQUIRE(group.has_value());

    // The same binding twice, then one unbind of the channel: both go, because unbind removes every
    // binding of that source on that channel — and the index must agree with the list.
    CY_REQUIRE(fixture.control.bind_entity(*source, channels::movement(), entity(1)).has_value());
    CY_REQUIRE(fixture.control.bind_entity(*source, channels::movement(), entity(1)).has_value());
    CY_CHECK(fixture.control.controls(*source, entity(1), channels::movement()));
    // A different channel, a different source and a different entity are all different keys.
    CY_CHECK_FALSE(fixture.control.controls(*source, entity(1), channels::turret()));
    CY_CHECK_FALSE(fixture.control.controls(*other, entity(1), channels::movement()));
    CY_CHECK_FALSE(fixture.control.controls(*source, entity(2), channels::movement()));
    fixture.control.unbind(*source, channels::movement());
    CY_CHECK_FALSE(fixture.control.controls(*source, entity(1), channels::movement()));

    // Through a group: membership removed, binding removed, source destroyed — each takes it away.
    CY_REQUIRE(fixture.control.add_to_group(*group, entity(3)).has_value());
    CY_REQUIRE(fixture.control.bind_group(*other, channels::command(), *group).has_value());
    CY_CHECK(fixture.control.controls(*other, entity(3), channels::command()));
    fixture.control.remove_from_group(*group, entity(3));
    CY_CHECK_FALSE(fixture.control.controls(*other, entity(3), channels::command()));
    CY_REQUIRE(fixture.control.add_to_group(*group, entity(3)).has_value());
    CY_CHECK(fixture.control.controls(*other, entity(3), channels::command()));
    fixture.control.destroy_source(*other);
    CY_CHECK_FALSE(fixture.control.controls(*other, entity(3), channels::command()));
}

CY_TEST_CASE("gameplay: an entity in more groups than the inline index is still answered exactly") {
    ControlFixture fixture;
    CY_REQUIRE(fixture.build());
    auto source = fixture.control.create_source(ControlSourceKind::Human, fixture.participant,
                                                cy::Name::intern("player"));
    CY_REQUIRE(source.has_value());
    constexpr u32 kGroups = ControlRegistry::kInlineMemberships + 3;
    GroupId groups[kGroups];
    for (GroupId& group : groups) {
        auto created = fixture.control.create_group(cy::Name::intern("overlap"));
        CY_REQUIRE(created.has_value());
        group = *created;
        CY_REQUIRE(fixture.control.add_to_group(group, entity(9)).has_value());
    }
    // Only the LAST group is bound: the inline slots hold the first four, so an index that trusted
    // them would answer no.
    CY_REQUIRE(
        fixture.control.bind_group(*source, channels::command(), groups[kGroups - 1]).has_value());
    CY_CHECK(fixture.control.controls(*source, entity(9), channels::command()));
    // And leaving that group, from the overflowed state, takes the answer away.
    fixture.control.remove_from_group(groups[kGroups - 1], entity(9));
    CY_CHECK_FALSE(fixture.control.controls(*source, entity(9), channels::command()));
    // Leaving groups until it fits inline again keeps every remaining membership exact.
    CY_REQUIRE(fixture.control.bind_group(*source, channels::command(), groups[0]).has_value());
    for (u32 index = 1; index + 1 < kGroups; ++index) {
        fixture.control.remove_from_group(groups[index], entity(9));
    }
    CY_CHECK(fixture.control.controls(*source, entity(9), channels::command()));
    fixture.control.remove_from_group(groups[0], entity(9));
    CY_CHECK_FALSE(fixture.control.controls(*source, entity(9), channels::command()));
}
