// M9 TASK 1.1 — the log seam: every command recorded once, in order, with its provenance.
//
// THE THREE CLAIMS, AND EACH IS A NUMBER RATHER THAN A READING:
//
//   once      `records_emitted()` equals `committed_count()` and equals the sink's own count. A
//             seam called twice, or called for a rejected command, changes one of the three.
//   in order  the sink's sequence of `(producer, sequence)` pairs is the merge order, compared
//             element by element against `committed(i)`.
//   with its  provenance arrives intact AND changes nothing: two commands differing only in
//   provenance provenance commit in the same order and are handed to the sink in the same order,
//             which is `gameplay-framework`'s "Provenance SHALL NOT affect validation, ordering, or
//             execution" and is what M8.c's firewall decision rests on.
//
// THE MUTATION THAT PROVES THESE ARE CHECKS. Calling the sink a second time in
// `CommandStream::commit()`, or moving the call above the validation branch, turns the first case
// red. The run is recorded in src/replay/README.md.

#include "fixture.h"

using namespace cy::gameplay_test;
using cy::i32;
using cy::u32;
using cy::u64;

namespace {

/// What a recorder on the other side of the seam sees. Deliberately not a `CommandLog`: the point
/// is to compare the seam against the stream's own list, and reusing the stream's container would
/// compare it against itself.
struct Recorder {
    static constexpr u32 kMax = 64;

    Command commands[kMax] = {};
    u32 count = 0;

    static void receive(void* user, const Command& command) noexcept {
        auto* self = static_cast<Recorder*>(user);
        if (self->count < kMax) {
            self->commands[self->count] = command;
        }
        ++self->count;
    }

    [[nodiscard]] CommandStream::RecordSink sink() noexcept {
        return CommandStream::RecordSink{&Recorder::receive, this};
    }
};

struct SeamFixture : Fixture {
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

CY_TEST_CASE("gameplay: the log seam sees every committed command exactly once, in merge order") {
    SeamFixture fixture;
    CY_REQUIRE(fixture.build());
    Recorder recorder;
    fixture.commands.set_record_sink(recorder.sink());
    CY_REQUIRE(fixture.commands.record_sink().bound());

    auto first = fixture.commands.open_producer(cy::Name::intern("a"));
    auto second = fixture.commands.open_producer(cy::Name::intern("b"));
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());

    // Two producers, three commands each, recorded interleaved. The merge is (producer, sequence),
    // so the seam must see producer a's three before producer b's three whatever order they were
    // recorded in.
    for (i32 index = 0; index < 3; ++index) {
        CY_REQUIRE(fixture.commands.producer(*second)
                       .record(fixture.move_command(100 + index))
                       .has_value());
        CY_REQUIRE(
            fixture.commands.producer(*first).record(fixture.move_command(index)).has_value());
    }

    fixture.commands.commit(fixture.context(), 7);

    CY_REQUIRE_EQ(fixture.commands.committed_count(), 6U);
    // ONCE. Three counts that would disagree if the seam fired twice, or fired for a rejection, or
    // did not fire at all.
    CY_CHECK_EQ(recorder.count, 6U);
    CY_CHECK_EQ(fixture.commands.records_emitted(), u64{6});
    CY_CHECK_EQ(fixture.commands.log().size(), 6U);

    // IN ORDER. Element by element against the stream's own committed list.
    for (u32 index = 0; index < 6; ++index) {
        const Command& committed = fixture.commands.committed(index);
        const Command& recorded = recorder.commands[index];
        CY_CHECK_EQ(recorded.sequence, committed.sequence);
        CY_CHECK_EQ(recorded.tick, committed.tick);
        CY_CHECK_EQ(recorded.type, committed.type);
        MoveIntent from_seam;
        MoveIntent from_stream;
        CY_REQUIRE(recorded.read_payload(from_seam));
        CY_REQUIRE(committed.read_payload(from_stream));
        CY_CHECK_EQ(from_seam.dx, from_stream.dx);
    }
    // The first three are producer a's — dx 0, 1, 2 — and the second three producer b's.
    for (u32 index = 0; index < 3; ++index) {
        MoveIntent intent;
        CY_REQUIRE(recorder.commands[index].read_payload(intent));
        CY_CHECK_EQ(intent.dx, static_cast<i32>(index));
    }
}

CY_TEST_CASE("gameplay: a rejected command never reaches the seam") {
    SeamFixture fixture;
    CY_REQUIRE(fixture.build());
    Recorder recorder;
    fixture.commands.set_record_sink(recorder.sink());

    auto producer = fixture.commands.open_producer(cy::Name::intern("a"));
    CY_REQUIRE(producer.has_value());

    // One legal command and one addressed to an entity this source does not control. The log is the
    // record of what the simulation *consumed*; a replay that re-ran rejected commands would depend
    // on the rejection reproducing identically, which is a second determinism obligation for no
    // benefit.
    CY_REQUIRE(fixture.commands.producer(*producer).record(fixture.move_command(1)).has_value());
    Command stray = fixture.move_command(2);
    stray.target = entity(99);
    CY_REQUIRE(fixture.commands.producer(*producer).record(stray).has_value());

    fixture.commands.commit(fixture.context(), 1);

    CY_REQUIRE_EQ(fixture.commands.rejection_count(), 1U);
    CY_CHECK_EQ(fixture.commands.committed_count(), 1U);
    CY_CHECK_EQ(recorder.count, 1U);
    CY_CHECK_EQ(fixture.commands.records_emitted(), u64{1});
}

CY_TEST_CASE("gameplay: provenance reaches the seam and changes nothing about the order") {
    // Two runs of the same three commands, differing only in provenance. `gameplay-framework`:
    // "Provenance SHALL NOT affect validation, ordering, or execution."
    const ControlSourceKind kinds[] = {ControlSourceKind::Human, ControlSourceKind::RemotePeer,
                                       ControlSourceKind::Replay};

    Recorder recorders[2];
    for (u32 run = 0; run < 2; ++run) {
        SeamFixture fixture;
        CY_REQUIRE(fixture.build());
        fixture.commands.set_record_sink(recorders[run].sink());
        auto producer = fixture.commands.open_producer(cy::Name::intern("a"));
        CY_REQUIRE(producer.has_value());
        for (u32 index = 0; index < 3; ++index) {
            Command command = fixture.move_command(static_cast<i32>(index));
            // Run 0 tags everything Human; run 1 tags each command differently.
            command.provenance =
                Provenance{run == 0 ? ControlSourceKind::Human : kinds[index], (run * 10) + index};
            CY_REQUIRE(fixture.commands.producer(*producer).record(command).has_value());
        }
        fixture.commands.commit(fixture.context(), 3);
        CY_REQUIRE_EQ(recorders[run].count, 3U);
    }

    for (u32 index = 0; index < 3; ++index) {
        MoveIntent left;
        MoveIntent right;
        CY_REQUIRE(recorders[0].commands[index].read_payload(left));
        CY_REQUIRE(recorders[1].commands[index].read_payload(right));
        // Same order, same payloads...
        CY_CHECK_EQ(left.dx, right.dx);
        CY_CHECK_EQ(recorders[0].commands[index].sequence, recorders[1].commands[index].sequence);
        // ...and the provenance travelled, intact and different, without moving anything.
        CY_CHECK(recorders[0].commands[index].provenance.kind == ControlSourceKind::Human);
        CY_CHECK(recorders[1].commands[index].provenance.kind == kinds[index]);
        CY_CHECK_EQ(recorders[1].commands[index].provenance.source, 10U + index);
    }
}

CY_TEST_CASE("gameplay: an unbound seam costs nothing and the stream still logs") {
    SeamFixture fixture;
    CY_REQUIRE(fixture.build());
    CY_CHECK_FALSE(fixture.commands.record_sink().bound());

    auto producer = fixture.commands.open_producer(cy::Name::intern("a"));
    CY_REQUIRE(producer.has_value());
    CY_REQUIRE(fixture.commands.producer(*producer).record(fixture.move_command(1)).has_value());
    fixture.commands.commit(fixture.context(), 1);

    CY_CHECK_EQ(fixture.commands.log().size(), 1U);
    // `records_emitted()` counts committed commands whether or not anyone was listening, so a
    // session that bound a sink late can tell how much it missed.
    CY_CHECK_EQ(fixture.commands.records_emitted(), u64{1});
}
