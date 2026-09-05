// TASK 4.4.1 — gameplay lifetimes, scoped services and the gameplay context.
//
// The two cases that carry the requirement are the ones about *survival*: participants surviving a
// world change, and session-scoped services dying with the session while application-scoped ones do
// not. Both are trivial to get right and trivial to lose — hang the participants off the world and
// the first one breaks the day a game gets a lobby.

#include "fixture.h"

using namespace cy::gameplay_test;
using cy::u32;

namespace {

/// Two services a case can register, distinct types so that the type tag is exercised rather than
/// asserted about.
struct MatchClock {
    u32 seconds = 0;
};
struct Progression {
    u32 level = 1;
};

}  // namespace

CY_TEST_CASE("gameplay: participants survive a world change") {
    Fixture fixture;
    auto lobby = fixture.session.add_world(nullptr, cy::Name::intern("lobby"));
    CY_REQUIRE(lobby.has_value());
    auto alice = fixture.session.add_participant(ParticipantKind::LocalHuman,
                                                 cy::Name::intern("alice"), 1, 0);
    auto bob =
        fixture.session.add_participant(ParticipantKind::RemoteHuman, cy::Name::intern("bob"), 2);
    CY_REQUIRE(alice.has_value());
    CY_REQUIRE(bob.has_value());
    CY_REQUIRE_EQ(fixture.session.participant_count(), 2);

    // Leave the lobby, enter the play world.
    fixture.session.remove_world(*lobby);
    auto play = fixture.session.add_world(nullptr, cy::Name::intern("play"));
    CY_REQUIRE(play.has_value());

    // The session did not end and neither did anybody in it. Identity, team and the seed are the
    // same objects, not rebuilt equivalents.
    CY_CHECK_EQ(fixture.session.participant_count(), 2);
    const Participant* still_alice = fixture.session.participant(*alice);
    CY_REQUIRE(still_alice != nullptr);
    CY_CHECK_EQ(still_alice->team, 1);
    CY_CHECK_EQ(fixture.session.seed(), 0x5EEDULL);
}

CY_TEST_CASE("gameplay: a session runs several worlds at once, each with a declared role") {
    Fixture fixture;
    auto primary = fixture.session.add_world(nullptr, cy::Name::intern("primary"));
    auto preview = fixture.session.add_world(nullptr, cy::Name::intern("preview"));
    CY_REQUIRE(primary.has_value());
    CY_REQUIRE(preview.has_value());
    CY_CHECK_EQ(fixture.session.world_count(), 2);

    const WorldSession* preview_session = fixture.session.world_session(*preview);
    CY_REQUIRE(preview_session != nullptr);
    // The role is a `Name`, so a project adds "spectator" or "replay" without an engine enumeration
    // gaining an enumerator.
    CY_CHECK(preview_session->role == cy::Name::intern("preview"));

    fixture.session.remove_world(*preview);
    CY_CHECK_EQ(fixture.session.world_count(), 1);
    CY_CHECK(fixture.session.world_session(*preview) == nullptr);
    CY_CHECK(fixture.session.world_session(*primary) != nullptr);
}

CY_TEST_CASE("gameplay: a remote participant needs no local resource") {
    Fixture fixture;
    // A local player index is offered and *ignored* for a remote participant. Not a convention —
    // the record cannot hold one, which is what makes "structural rather than a special case" true.
    auto remote = fixture.session.add_participant(ParticipantKind::RemoteHuman,
                                                  cy::Name::intern("far"), 0, 3);
    CY_REQUIRE(remote.has_value());
    const Participant* record = fixture.session.participant(*remote);
    CY_REQUIRE(record != nullptr);
    CY_CHECK_EQ(record->local_player, kNoLocalPlayer);

    auto local = fixture.session.add_participant(ParticipantKind::LocalHuman,
                                                 cy::Name::intern("here"), 0, 3);
    CY_REQUIRE(local.has_value());
    CY_CHECK_EQ(fixture.session.participant(*local)->local_player, 3);
}

CY_TEST_CASE("gameplay: scope determines a service's lifetime") {
    Fixture fixture;
    MatchClock clock;
    Progression progression;
    ServiceRegistry& services = fixture.session.services();

    CY_REQUIRE(services.add(Scope::Session, cy::Name::intern("clock"), &clock).has_value());
    CY_REQUIRE(services.add(Scope::Application, cy::Name::intern("progression"), &progression)
                   .has_value());
    CY_CHECK_EQ(services.count_at(Scope::Session), 1);
    CY_CHECK_EQ(services.count_at(Scope::Application), 1);

    services.clear_scope(Scope::Session);
    // The session's services are gone; the application's are untouched.
    CY_CHECK(services.find<MatchClock>(Scope::Session, cy::Name::intern("clock")) == nullptr);
    CY_CHECK(services.find<Progression>(Scope::Application, cy::Name::intern("progression")) ==
             &progression);
}

CY_TEST_CASE("gameplay: a service is found by scope, name and type") {
    Fixture fixture;
    MatchClock session_clock;
    MatchClock world_clock;
    ServiceRegistry& services = fixture.session.services();
    // One name at two scopes is legitimate — a clock per session and a clock per world — and the
    // two must not collide.
    CY_REQUIRE(services.add(Scope::Session, cy::Name::intern("clock"), &session_clock).has_value());
    CY_REQUIRE(services.add(Scope::World, cy::Name::intern("clock"), &world_clock).has_value());
    CY_CHECK(services.find<MatchClock>(Scope::Session, cy::Name::intern("clock")) ==
             &session_clock);
    CY_CHECK(services.find<MatchClock>(Scope::World, cy::Name::intern("clock")) == &world_clock);

    // The wrong type does not silently reinterpret the pointer. Without RTTI a `void*` cast would
    // have compiled and produced garbage; the type tag is the `-fno-rtti` answer.
    CY_CHECK(services.find<Progression>(Scope::Session, cy::Name::intern("clock")) == nullptr);

    // A duplicate at one scope is refused rather than replaced: which one a system gets would
    // otherwise depend on registration order.
    CY_CHECK_FALSE(
        services.add(Scope::Session, cy::Name::intern("clock"), &world_clock).has_value());
}

CY_TEST_CASE(
    "gameplay: everything a system needs is reachable from the context, and nothing else") {
    Fixture fixture;
    auto world = fixture.session.add_world(nullptr, cy::Name::intern("primary"));
    CY_REQUIRE(world.has_value());
    fixture.tick = 42;

    GameplayContext context = fixture.context();
    context.world_session = fixture.session.world_session(*world);
    CY_CHECK(context.valid());
    CY_CHECK(context.session == &fixture.session);
    CY_CHECK(context.commands == &fixture.commands);
    CY_CHECK(context.services == &fixture.session.services());
    CY_CHECK_EQ(context.at.tick, 42);
    // A moment is (epoch, tick), never a tick alone: two occurrences of one tick after a rollback
    // are distinguishable only by the epoch.
    CY_CHECK_EQ(context.at.epoch.value, 0);
}

CY_TEST_CASE("gameplay: the phase is a name, extensible without an engine enumeration") {
    Fixture fixture;
    fixture.session.set_phase(cy::Name::intern("Match.Warmup"));
    CY_CHECK(fixture.session.phase() == cy::Name::intern("Match.Warmup"));
    // A game adding overtime adds a name and a transition rule; nothing in the engine changes.
    fixture.session.set_phase(cy::Name::intern("Match.Overtime"));
    CY_CHECK(fixture.session.phase() == cy::Name::intern("Match.Overtime"));
}

CY_TEST_CASE("gameplay: every enumerator has a spelling a diagnostic can print") {
    for (u32 index = 0; index < static_cast<u32>(Scope::Count); ++index) {
        CY_CHECK_NE(scope_name(static_cast<Scope>(index))[0], '\0');
    }
    for (u32 index = 0; index < static_cast<u32>(ParticipantKind::Count); ++index) {
        CY_CHECK_NE(participant_kind_name(static_cast<ParticipantKind>(index))[0], '\0');
    }
    for (u32 index = 0; index < static_cast<u32>(ControlSourceKind::Count); ++index) {
        CY_CHECK_NE(control_source_kind_name(static_cast<ControlSourceKind>(index))[0], '\0');
    }
    for (u32 index = 0; index < static_cast<u32>(ReasonTag::Count); ++index) {
        CY_CHECK_NE(reason_tag_name(static_cast<ReasonTag>(index))[0], '\0');
    }
}
