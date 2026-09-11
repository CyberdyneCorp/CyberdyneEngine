// A sequence is the sixth command producer, and the simulation cannot tell.
//
// The two cases that carry the requirement are the last two: a sequence-issued command and a
// player-issued one of the same type VALIDATE IDENTICALLY, and they commit in the order their
// producers were registered rather than in any order that depends on where they came from.

#include "fixture.h"

#include <cy/gameplay/context.h>
#include <cy/gameplay/control.h>
#include <cy/sequencing/gameplay/commands.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::sequencing;
using namespace cy::sequencing::testing;

namespace {

constexpr u32 kUnlockStableId = 4242;

struct GameplayFixture {
    GameplayFixture() noexcept
        : session(allocator(), 0x5EED),
          control(allocator()),
          commands(allocator(), control),
          bridge(allocator(), commands) {}

    [[nodiscard]] Status build() noexcept {
        const Expected<gameplay::ParticipantId, Error> added =
            session.add_participant(gameplay::ParticipantKind::LocalHuman, Name::intern("player"));
        if (!added) {
            return Status{make_unexpected(added.error())};
        }
        participant = added.value();

        const Expected<gameplay::ControlSourceId, Error> human = control.create_source(
            gameplay::ControlSourceKind::Human, participant, Name::intern("input"));
        if (!human) {
            return Status{make_unexpected(human.error())};
        }
        human_source = human.value();

        // The sequence's own control source. `Script` is what a sequence is; it acts for the same
        // participant, exactly as an assist AI does.
        const Expected<gameplay::ControlSourceId, Error> script = control.create_source(
            gameplay::ControlSourceKind::Script, participant, Name::intern("sequence"));
        if (!script) {
            return Status{make_unexpected(script.error())};
        }
        script_source = script.value();

        if (!control.bind_entity(human_source, gameplay::channels::command(),
                                 ecs::Entity::make(1, 1))) {
            return fail(ErrorCode::Internal, "human binding");
        }
        if (!control.bind_entity(script_source, gameplay::channels::command(),
                                 ecs::Entity::make(1, 1))) {
            return fail(ErrorCode::Internal, "script binding");
        }

        gameplay::CommandDeclaration declaration;
        declaration.name = Name::intern("Unlock");
        declaration.stable_id = kUnlockStableId;
        declaration.channel = gameplay::channels::command();
        const Expected<gameplay::CommandTypeId, Error> declared = commands.declare(declaration);
        if (!declared) {
            return Status{make_unexpected(declared.error())};
        }
        unlock = declared.value();
        return bridge.initialize(Name::intern("sequence"), script_source, participant);
    }

    [[nodiscard]] gameplay::GameplayContext context() noexcept {
        gameplay::GameplayContext ctx;
        ctx.session = &session;
        ctx.services = &session.services();
        ctx.commands = &commands;
        ctx.at.tick = 1;
        return ctx;
    }

    gameplay::GameSession session;
    gameplay::ControlRegistry control;
    gameplay::CommandStream commands;
    CommandBridge bridge;
    gameplay::ParticipantId participant;
    gameplay::ControlSourceId human_source;
    gameplay::ControlSourceId script_source;
    gameplay::CommandTypeId unlock = gameplay::kInvalidCommandType;
};

[[nodiscard]] CommandRequest unlock_request(u32 instance, u32 track) noexcept {
    CommandRequest request;
    request.command_stable_id = kUnlockStableId;
    request.target = ecs::Entity::make(1, 1).bits();
    request.payload_size = 2;
    request.payload[0] = 7;
    request.provenance.instance = instance;
    request.provenance.track_stable_id = track;
    return request;
}

}  // namespace

CY_TEST_CASE("sequence_commands: a sequence-issued command is an ordinary command") {
    GameplayFixture fixture;
    CY_REQUIRE(fixture.build());

    const CommandRequest request = unlock_request(3, 200);
    CommandBridgeReport report;
    CY_REQUIRE(fixture.bridge.submit(Span<const CommandRequest>(&request, 1), 1, report));
    CY_CHECK_EQ(report.submitted, 1U);

    fixture.commands.commit(fixture.context(), 1);
    CY_REQUIRE_EQ(fixture.commands.committed_count(), 1U);
    const gameplay::Command& committed = fixture.commands.committed(0);
    CY_CHECK_EQ(committed.type, fixture.unlock);
    CY_CHECK_EQ(committed.payload[0], 7);
    // Provenance is recorded and is diagnostics only.
    CY_CHECK_EQ(committed.provenance.kind, gameplay::ControlSourceKind::Script);

    // The sequence and track identity travel BESIDE the command, joined on its sequence number.
    CY_REQUIRE_EQ(fixture.bridge.notes().size(), 1U);
    CY_CHECK_EQ(fixture.bridge.notes()[0].instance, 3U);
    CY_CHECK_EQ(fixture.bridge.notes()[0].track_stable_id, 200U);
    CY_CHECK_EQ(fixture.bridge.notes()[0].command_sequence, committed.sequence);
}

CY_TEST_CASE("sequence_commands: a command type nobody declared is counted, not invented") {
    GameplayFixture fixture;
    CY_REQUIRE(fixture.build());
    CommandRequest request = unlock_request(1, 1);
    request.command_stable_id = 999;  // no declaration
    CommandBridgeReport report;
    CY_REQUIRE(fixture.bridge.submit(Span<const CommandRequest>(&request, 1), 1, report));
    CY_CHECK_EQ(report.unknown_types, 1U);
    CY_CHECK_EQ(report.submitted, 0U);
}

CY_TEST_CASE("sequence_commands: the simulation cannot tell") {
    // "**WHEN** a sequence issues a command **THEN** it SHALL be validated and executed exactly as
    // a player's or an agent's would be."
    GameplayFixture fixture;
    CY_REQUIRE(fixture.build());

    // The player's own producer, submitting the identical command through the ordinary path.
    const Expected<u32, Error> human_producer =
        fixture.commands.open_producer(Name::intern("input"));
    CY_REQUIRE(human_producer);

    gameplay::Command by_hand;
    by_hand.type = fixture.unlock;
    by_hand.tick = 1;
    by_hand.participant = fixture.participant;
    by_hand.source = fixture.human_source;
    by_hand.target = ecs::Entity::make(1, 1);
    by_hand.payload_size = 2;
    by_hand.payload[0] = 7;

    const gameplay::ValidationResult human_result =
        fixture.commands.validate(fixture.context(), by_hand);

    const CommandRequest request = unlock_request(3, 200);
    CommandBridgeReport report;
    CY_REQUIRE(fixture.bridge.submit(Span<const CommandRequest>(&request, 1), 1, report));
    const gameplay::Command& from_sequence =
        fixture.commands.producer(fixture.bridge.producer()).at(0);
    const gameplay::ValidationResult sequence_result =
        fixture.commands.validate(fixture.context(), from_sequence);

    CY_CHECK_EQ(human_result.permitted(), sequence_result.permitted());
    CY_CHECK_EQ(static_cast<u32>(human_result.first().tag),
                static_cast<u32>(sequence_result.first().tag));
    CY_CHECK(sequence_result.permitted());
}

CY_TEST_CASE("sequence_commands: provenance does not change the order") {
    // Two producers, one command each, and the merge key is (producer order, sequence) — never the
    // provenance and never a thread.
    GameplayFixture fixture;
    CY_REQUIRE(fixture.build());
    const Expected<u32, Error> human_producer =
        fixture.commands.open_producer(Name::intern("input"));
    CY_REQUIRE(human_producer);

    const CommandRequest request = unlock_request(3, 200);
    CommandBridgeReport report;
    CY_REQUIRE(fixture.bridge.submit(Span<const CommandRequest>(&request, 1), 1, report));

    gameplay::Command by_hand;
    by_hand.type = fixture.unlock;
    by_hand.tick = 1;
    by_hand.participant = fixture.participant;
    by_hand.source = fixture.human_source;
    by_hand.target = ecs::Entity::make(1, 1);
    by_hand.payload_size = 2;
    by_hand.payload[0] = 7;
    CY_REQUIRE(fixture.commands.producer(human_producer.value()).record(by_hand));

    fixture.commands.commit(fixture.context(), 1);
    CY_REQUIRE_EQ(fixture.commands.committed_count(), 2U);
    // The sequence's producer was opened first, so its command commits first — because of the
    // registration order and for no other reason.
    CY_CHECK_EQ(fixture.commands.committed(0).provenance.kind, gameplay::ControlSourceKind::Script);
    CY_CHECK_EQ(fixture.commands.committed(1).provenance.kind, gameplay::ControlSourceKind::Human);
}
