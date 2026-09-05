// TASK 4.4.3 — one command stream: the simulation's only input.
//
// The requirement's own scenarios, plus the two properties that make replay possible at all: the
// merge order is deterministic and depends on nothing but (producer, sequence), and provenance
// changes nothing.

#include "fixture.h"

using namespace cy::gameplay_test;
using cy::i32;
using cy::u32;
using cy::u64;

namespace {

struct StreamFixture : Fixture {
    [[nodiscard]] bool build() noexcept {
        auto added =
            session.add_participant(ParticipantKind::LocalHuman, cy::Name::intern("player"));
        if (!added) {
            return false;
        }
        participant = *added;
        auto created =
            control.create_source(ControlSourceKind::Human, participant, cy::Name::intern("input"));
        if (!created) {
            return false;
        }
        source = *created;
        if (!control.bind_entity(source, channels::movement(), entity(1)).has_value()) {
            return false;
        }
        CommandDeclaration declaration;
        declaration.name = cy::Name::intern("Move");
        declaration.stable_id = 11;
        declaration.channel = channels::movement();
        auto declared = commands.declare(declaration);
        if (!declared) {
            return false;
        }
        move = *declared;
        return true;
    }

    [[nodiscard]] Command move_command(i32 dx) const noexcept {
        Command command;
        command.type = move;
        command.participant = participant;
        command.source = source;
        command.target = entity(1);
        (void)command.set_payload(MoveIntent{dx, 0});
        return command;
    }

    ParticipantId participant;
    ControlSourceId source;
    CommandTypeId move = kInvalidCommandType;
};

}  // namespace

CY_TEST_CASE("gameplay: a command type is identified by a stable id, not by its name") {
    StreamFixture fixture;
    CY_REQUIRE(fixture.build());
    CY_CHECK_EQ(fixture.commands.find(11), fixture.move);
    CY_CHECK_EQ(fixture.commands.declaration(fixture.move).schema_version, 1);

    CommandDeclaration duplicate;
    duplicate.name = cy::Name::intern("Walk");
    duplicate.stable_id = 11;
    CY_CHECK_FALSE(fixture.commands.declare(duplicate).has_value());

    CommandDeclaration unnamed;
    unnamed.name = cy::Name::intern("Nothing");
    CY_CHECK_FALSE(fixture.commands.declare(unnamed).has_value());
}

CY_TEST_CASE("gameplay: four producers are indistinguishable to the simulation") {
    StreamFixture fixture;
    CY_REQUIRE(fixture.build());
    // A human, an AI, a network peer and a replay. `gameplay-framework` — "Origin is
    // indistinguishable": the simulation processes all four identically.
    const ControlSourceKind kinds[] = {ControlSourceKind::Human,
                                       ControlSourceKind::ArtificialIntelligence,
                                       ControlSourceKind::RemotePeer, ControlSourceKind::Replay};
    for (u32 index = 0; index < 4; ++index) {
        auto producer = fixture.commands.open_producer(cy::Name::intern("producer"));
        CY_REQUIRE(producer.has_value());
        Command command = fixture.move_command(static_cast<i32>(index) + 1);
        command.provenance = Provenance{kinds[index], index};
        CY_REQUIRE(fixture.commands.producer(*producer).record(command).has_value());
    }
    fixture.commands.commit(fixture.context(), 1);

    CY_REQUIRE_EQ(fixture.commands.committed_count(), 4);
    for (u32 index = 0; index < 4; ++index) {
        MoveIntent intent;
        CY_REQUIRE(fixture.commands.committed(index).read_payload(intent));
        // Committed in producer order, and all four permitted: nothing about the origin entered the
        // decision or the ordering.
        CY_CHECK_EQ(intent.dx, static_cast<i32>(index) + 1);
    }
}

CY_TEST_CASE("gameplay: provenance changes neither validation nor order nor the log's identity") {
    StreamFixture first;
    StreamFixture second;
    CY_REQUIRE(first.build());
    CY_REQUIRE(second.build());
    auto producer_a = first.commands.open_producer(cy::Name::intern("a"));
    auto producer_b = second.commands.open_producer(cy::Name::intern("b"));
    CY_REQUIRE(producer_a.has_value());
    CY_REQUIRE(producer_b.has_value());

    for (u32 index = 0; index < 5; ++index) {
        Command human = first.move_command(static_cast<i32>(index));
        human.provenance = Provenance{ControlSourceKind::Human, 1};
        CY_REQUIRE(first.commands.producer(*producer_a).record(human).has_value());

        Command replay = second.move_command(static_cast<i32>(index));
        replay.provenance = Provenance{ControlSourceKind::Replay, 99};
        CY_REQUIRE(second.commands.producer(*producer_b).record(replay).has_value());
    }
    first.commands.commit(first.context(), 7);
    second.commands.commit(second.context(), 7);

    CY_CHECK_EQ(first.commands.committed_count(), second.commands.committed_count());
    // The same identity. `gameplay-framework`: "Provenance SHALL NOT affect validation, ordering,
    // or execution" — and a log hash that included it would make a replay's log differ from the run
    // it reproduces, which is the first thing anybody would "fix" the wrong way.
    CY_CHECK_EQ(first.commands.log().hash(), second.commands.log().hash());
}

CY_TEST_CASE("gameplay: the merge order is (producer, sequence) and nothing else") {
    StreamFixture fixture;
    CY_REQUIRE(fixture.build());
    auto slow = fixture.commands.open_producer(cy::Name::intern("slow"));
    auto fast = fixture.commands.open_producer(cy::Name::intern("fast"));
    CY_REQUIRE(slow.has_value());
    CY_REQUIRE(fast.has_value());

    // Recorded in an interleaved order that a threaded run would not reproduce. The commit does not
    // care: producer 0's commands come first, in its own record order, then producer 1's.
    CY_REQUIRE(fixture.commands.producer(*fast).record(fixture.move_command(10)).has_value());
    CY_REQUIRE(fixture.commands.producer(*slow).record(fixture.move_command(1)).has_value());
    CY_REQUIRE(fixture.commands.producer(*fast).record(fixture.move_command(11)).has_value());
    CY_REQUIRE(fixture.commands.producer(*slow).record(fixture.move_command(2)).has_value());

    fixture.commands.commit(fixture.context(), 3);
    CY_REQUIRE_EQ(fixture.commands.committed_count(), 4);
    const i32 expected[] = {1, 2, 10, 11};
    for (u32 index = 0; index < 4; ++index) {
        MoveIntent intent;
        CY_REQUIRE(fixture.commands.committed(index).read_payload(intent));
        CY_CHECK_EQ(intent.dx, expected[index]);
        CY_CHECK_EQ(fixture.commands.committed(index).tick, 3);
    }
}

CY_TEST_CASE("gameplay: a rejected command is not committed and not logged") {
    StreamFixture fixture;
    CY_REQUIRE(fixture.build());
    auto producer = fixture.commands.open_producer(cy::Name::intern("input"));
    CY_REQUIRE(producer.has_value());

    Command illegal = fixture.move_command(1);
    illegal.target = entity(9);  // not controlled by this source
    CY_REQUIRE(fixture.commands.producer(*producer).record(illegal).has_value());
    CY_REQUIRE(fixture.commands.producer(*producer).record(fixture.move_command(2)).has_value());
    fixture.commands.commit(fixture.context(), 1);

    CY_CHECK_EQ(fixture.commands.committed_count(), 1);
    CY_CHECK_EQ(fixture.commands.rejection_count(), 1);
    // The log is the record of what the simulation *consumed*. Logging rejections would make a
    // replay depend on the rejection being reproduced identically — a second determinism obligation
    // for no benefit.
    CY_CHECK_EQ(fixture.commands.log().size(), 1);
    CY_CHECK(fixture.commands.rejection(0).result.has(ReasonTag::NotControlled));
}

CY_TEST_CASE("gameplay: a payload is intent, and reading it back checks the size") {
    Command command;
    CY_REQUIRE(command.set_payload(MoveIntent{3, -4}));
    MoveIntent intent;
    CY_REQUIRE(command.read_payload(intent));
    CY_CHECK_EQ(intent.dx, 3);
    CY_CHECK_EQ(intent.dy, -4);

    // A different type of a different size does not silently reinterpret the bytes. Same size would
    // — the payload is opaque bytes and the command type is what says how to read them; this check
    // catches the accident, not the lie.
    struct Bigger {
        cy::i64 a = 0;
        cy::i64 b = 0;
        cy::i64 c = 0;
    };
    Bigger bigger;
    CY_CHECK_FALSE(command.read_payload(bigger));
}

CY_TEST_CASE("gameplay: a group command reaches only the members that can accept it") {
    StreamFixture fixture;
    CY_REQUIRE(fixture.build());
    auto producer = fixture.commands.open_producer(cy::Name::intern("input"));
    CY_REQUIRE(producer.has_value());

    constexpr u32 kBuildCapable = 1U << 0U;
    CommandDeclaration build;
    build.name = cy::Name::intern("Build");
    build.stable_id = 12;
    build.channel = channels::command();
    build.required_capability = kBuildCapable;
    auto declared = fixture.commands.declare(build);
    CY_REQUIRE(declared.has_value());

    auto group = fixture.control.create_group(cy::Name::intern("selection"));
    CY_REQUIRE(group.has_value());
    for (u32 index = 10; index < 14; ++index) {
        CY_REQUIRE(fixture.control.add_to_group(*group, entity(index)).has_value());
    }
    CY_REQUIRE(fixture.control.bind_group(fixture.source, channels::command(), *group).has_value());
    // Two builders and two soldiers.
    CY_REQUIRE(fixture.commands.set_capabilities(entity(10), kBuildCapable).has_value());
    CY_REQUIRE(fixture.commands.set_capabilities(entity(11), kBuildCapable).has_value());

    Command prototype;
    prototype.type = *declared;
    prototype.participant = fixture.participant;
    prototype.source = fixture.source;
    prototype.group = *group;
    (void)prototype.set_payload(MoveIntent{0, 0});

    cy::Array<Entity> excluded(allocator());
    CY_REQUIRE(fixture.commands.route_to_group(prototype, *producer, excluded).has_value());
    fixture.commands.commit(fixture.context(), 1);

    CY_CHECK_EQ(fixture.commands.committed_count(), 2);
    // The excluded members are *reported*. `gameplay-framework`'s "A mixed selection": the
    // exclusion is something the interface can say out loud rather than a silence the player has to
    // interpret.
    CY_REQUIRE_EQ(excluded.size(), 2);
    CY_CHECK(excluded[0] == entity(12));
    CY_CHECK(excluded[1] == entity(13));
}

CY_TEST_CASE("gameplay: a command declares its reliability, prediction and locality") {
    StreamFixture fixture;
    CY_REQUIRE(fixture.build());
    CommandDeclaration ping;
    ping.name = cy::Name::intern("CameraShake");
    ping.stable_id = 21;
    ping.local_only = true;
    ping.authoritative = false;
    ping.reliability = Reliability::Unreliable;
    auto declared = fixture.commands.declare(ping);
    CY_REQUIRE(declared.has_value());

    const CommandDeclaration& stored = fixture.commands.declaration(*declared);
    // Declared per type, because "fire" and "set the build queue" have different answers and a
    // transport that guessed would be wrong for one of them.
    CY_CHECK(stored.local_only);
    CY_CHECK_FALSE(stored.authoritative);
    CY_CHECK_EQ(stored.reliability, Reliability::Unreliable);
    // A command with no channel is a session-level command and needs no controlled target — which
    // is how a camera shake or a UI ping is expressible without inventing a fake entity.
    CY_CHECK(stored.channel.is_empty());

    auto producer = fixture.commands.open_producer(cy::Name::intern("ui"));
    CY_REQUIRE(producer.has_value());
    Command command;
    command.type = *declared;
    command.participant = fixture.participant;
    CY_REQUIRE(fixture.commands.producer(*producer).record(command).has_value());
    fixture.commands.commit(fixture.context(), 1);
    CY_CHECK_EQ(fixture.commands.committed_count(), 1);
}
