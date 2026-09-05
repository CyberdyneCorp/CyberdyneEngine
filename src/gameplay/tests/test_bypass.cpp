// TASK 4.4.6 — THE TEST THAT BYPASSES THE STREAM, AND FAILS.
//
// ================================================================================================
// WHAT THIS FILE IS FOR
// ================================================================================================
//
// design.md §3, in full, because everything below is an execution of it:
//
//   "Input reaches simulation **only** as commands. There is no 'read the input state in a system'
//    path, and no tool that pokes simulation state directly.
//
//    This is the invariant the roadmap pins to M4, and it is worth being precise about why. Replay,
//    rollback and lockstep are not three mechanisms — they are one command log read three ways.
//    That is only true if the log is complete. A single system that reads a device directly does
//    not merely bypass the stream; it makes the M9 guarantees **unachievable** until someone finds
//    and removes it, and nothing will point at it, because everything will appear to work until a
//    desync months later.
//
//    Write the test that bypasses the stream and fails."
//
// Three of them, at three different depths, because the invariant can be broken in three ways:
//
//   1. THE STRUCTURAL TEST. A gameplay translation unit cannot even *include* an input header,
//      because `src/gameplay/` declares no dependency on `cy::servers-input` and the header is
//      therefore not on the include path. `__has_include` asserts it. Adding the dependency turns
//      this test red, which is the point: the dependency is the thing to notice.
//
//   2. THE SHAPE TEST. `GameplayContext` — the only thing a system is handed — exposes no way to
//      reach a device, an action or an input server. A concept asserts that no such member exists,
//      so adding one is a compile failure here rather than a convenience nobody questions.
//
//   3. THE CONSEQUENCE TEST, which is the one that matters and the one that teaches. A simulation
//      that bypasses the stream *works perfectly* while it runs. It fails only on replay, and only
//      in the half that bypassed. The case below runs both, side by side, on the same inputs, and
//      shows the conforming one reproducing bit-for-bit while the bypassing one silently loses
//      exactly the motion that never became a command.
//
// The third case is deliberately built as a **partial** bypass — one axis through the stream, one
// axis around it — because that is what the defect looks like in practice. Nobody writes a system
// that ignores commands entirely; somebody adds one convenient read for one value, and everything
// keeps working.

#include "fixture.h"

// (1) THE STRUCTURAL TEST. Not a runtime assertion — a compile-time one, evaluated by the
// preprocessor against this translation unit's actual include path.
//
// If someone adds `cy::servers-input` to src/gameplay/CMakeLists.txt's dependency list, this header
// becomes reachable and the static_assert below fails with the message that explains why that is
// not a build fix. There is no way to satisfy both this file and that dependency, which is exactly
// the property design.md §3 asks for.
#if __has_include(<cy/servers/input/server.h>)
static_assert(false,
              "src/gameplay/ can see an input header. That means cy::servers-input was added to "
              "this module's dependencies, and design.md 3 says it must not be: input reaches "
              "simulation ONLY as commands. The bridge from actions to commands belongs above both "
              "modules, where it can see each of them once, not inside gameplay where every system "
              "could reach a device. Remove the dependency; do not delete this check.");
#endif

using namespace cy::gameplay_test;
using cy::i32;
using cy::u32;
using cy::u64;

namespace {

// (2) THE SHAPE TEST. Concepts over `GameplayContext`, so that "the context carries no input" is
// checked by the compiler rather than by a reader's memory.
template <class T>
concept HasInputMember = requires(T context) { context.input; };
template <class T>
concept HasDeviceMember = requires(T context) { context.devices; };
template <class T>
concept HasActionMember = requires(T context) { context.actions; };

static_assert(!HasInputMember<GameplayContext>,
              "GameplayContext must expose no input. The context is the only thing a gameplay "
              "system is handed, so what is absent here is absent everywhere.");
static_assert(!HasDeviceMember<GameplayContext>, "GameplayContext must expose no devices.");
static_assert(!HasActionMember<GameplayContext>, "GameplayContext must expose no actions.");

// --- The little simulation the third case drives -------------------------------------------------

/// Four entities with an integer position. Integers, not floats, so that a divergence is a
/// divergence rather than an argument about tolerance.
struct MiniWorld {
    static constexpr u32 kEntities = 4;
    i32 x[kEntities] = {};
    i32 y[kEntities] = {};

    [[nodiscard]] u64 hash() const noexcept {
        u64 value = 1469598103934665603ULL;
        for (u32 index = 0; index < kEntities; ++index) {
            value = (value ^ static_cast<u64>(static_cast<u32>(x[index]))) * 1099511628211ULL;
            value = (value ^ static_cast<u64>(static_cast<u32>(y[index]))) * 1099511628211ULL;
        }
        return value;
    }
};

/// The side channel: a stand-in for a device the simulation must not read. In a real engine this is
/// an `InputServer`; here it is a table, because the point is not what it is but that a system can
/// see it at all.
struct SideChannel {
    [[nodiscard]] static i32 dx_at(u64 tick) noexcept { return static_cast<i32>(tick % 3) - 1; }
    /// Deliberately never zero and never negative. A signal that summed to zero over the run
    /// would let the bypassing case pass by *coincidence* — the divergence would be invisible for
    /// exactly the entities whose deltas happened to cancel, which is the shape of a test that goes
    /// green for the wrong reason.
    [[nodiscard]] static i32 dy_at(u64 tick) noexcept { return static_cast<i32>(tick % 3) + 1; }
};

/// Apply one committed command to the world. **The only way the world changes** in the conforming
/// run — which is what makes the log complete.
void apply(MiniWorld& world, const Command& command) noexcept {
    MoveIntent intent;
    if (!command.read_payload(intent)) {
        return;
    }
    const u32 index = command.target.index();
    if (index >= MiniWorld::kEntities) {
        return;
    }
    world.x[index] += intent.dx;
    world.y[index] += intent.dy;
}

/// A session with one participant, one source, four controlled entities and one command type.
struct Simulation {
    Simulation() noexcept : fixture(0xC0FFEEULL) {}

    [[nodiscard]] bool build() noexcept {
        auto added = fixture.session.add_participant(ParticipantKind::LocalHuman,
                                                     cy::Name::intern("player"));
        if (!added) {
            return false;
        }
        participant = *added;
        auto created = fixture.control.create_source(ControlSourceKind::Human, participant,
                                                     cy::Name::intern("player-input"));
        if (!created) {
            return false;
        }
        source = *created;
        for (u32 index = 0; index < MiniWorld::kEntities; ++index) {
            if (!fixture.control.bind_entity(source, channels::movement(), entity(index))
                     .has_value()) {
                return false;
            }
        }
        CommandDeclaration declaration;
        declaration.name = cy::Name::intern("Move");
        declaration.stable_id = 1;
        declaration.channel = channels::movement();
        auto declared = fixture.commands.declare(declaration);
        if (!declared) {
            return false;
        }
        move = *declared;
        auto opened = fixture.commands.open_producer(cy::Name::intern("human"));
        if (!opened) {
            return false;
        }
        producer = *opened;
        return true;
    }

    [[nodiscard]] Command move_command(u32 target, i32 dx, i32 dy) const noexcept {
        Command command;
        command.type = move;
        command.participant = participant;
        command.source = source;
        command.target = entity(target);
        (void)command.set_payload(MoveIntent{dx, dy});
        return command;
    }

    Fixture fixture;
    ParticipantId participant;
    ControlSourceId source;
    CommandTypeId move = kInvalidCommandType;
    u32 producer = 0;
};

constexpr u64 kTicks = 24;

}  // namespace

CY_TEST_CASE("gameplay: a conforming run replays from its command log, bit for bit") {
    Simulation live;
    CY_REQUIRE(live.build());
    MiniWorld world;

    // THE CONFORMING SHAPE. The side channel is read by the *producer*, which turns it into a
    // command; the simulation reads only committed commands. That is the whole of design.md §3.
    for (u64 tick = 1; tick <= kTicks; ++tick) {
        for (u32 index = 0; index < MiniWorld::kEntities; ++index) {
            const Command command = live.move_command(index, SideChannel::dx_at(tick + index),
                                                      SideChannel::dy_at(tick + index));
            CY_REQUIRE(live.fixture.commands.producer(live.producer).record(command).has_value());
        }
        live.fixture.commands.commit(live.fixture.context(), tick);
        for (u32 index = 0; index < live.fixture.commands.committed_count(); ++index) {
            apply(world, live.fixture.commands.committed(index));
        }
    }

    const u64 live_hash = world.hash();
    CY_REQUIRE_EQ(live.fixture.commands.log().size(), kTicks * MiniWorld::kEntities);

    // Replay: a fresh world, and nothing but the log. No side channel is consulted.
    MiniWorld replayed;
    for (u32 index = 0; index < live.fixture.commands.log().size(); ++index) {
        apply(replayed, live.fixture.commands.log().at(index));
    }

    CY_CHECK_EQ(replayed.hash(), live_hash);
    for (u32 index = 0; index < MiniWorld::kEntities; ++index) {
        CY_CHECK_EQ(replayed.x[index], world.x[index]);
        CY_CHECK_EQ(replayed.y[index], world.y[index]);
    }
}

CY_TEST_CASE("gameplay: a system that reads input directly makes its replay diverge") {
    Simulation bypassing;
    CY_REQUIRE(bypassing.build());
    MiniWorld world;

    // THE BYPASS, AND NOTE HOW SMALL IT IS. X still goes through the command stream. Y is read from
    // the side channel by the simulation itself — one convenient line, of the kind somebody adds
    // because the value was right there.
    //
    // Everything about this run looks correct while it runs: the entities move, the commands
    // validate, the log fills up, no assertion anywhere fires.
    for (u64 tick = 1; tick <= kTicks; ++tick) {
        for (u32 index = 0; index < MiniWorld::kEntities; ++index) {
            const Command command =
                bypassing.move_command(index, SideChannel::dx_at(tick + index), 0);
            CY_REQUIRE(bypassing.fixture.commands.producer(bypassing.producer)
                           .record(command)
                           .has_value());
        }
        bypassing.fixture.commands.commit(bypassing.fixture.context(), tick);
        for (u32 index = 0; index < bypassing.fixture.commands.committed_count(); ++index) {
            apply(world, bypassing.fixture.commands.committed(index));
        }
        // The bypass.
        for (u32 index = 0; index < MiniWorld::kEntities; ++index) {
            world.y[index] += SideChannel::dy_at(tick + index);
        }
    }

    const u64 live_hash = world.hash();

    // Replay from the log, exactly as the conforming case did.
    MiniWorld replayed;
    for (u32 index = 0; index < bypassing.fixture.commands.log().size(); ++index) {
        apply(replayed, bypassing.fixture.commands.log().at(index));
    }

    // ============================================================================================
    // THE FAILURE, MADE VISIBLE
    // ============================================================================================
    //
    // The replay does not reproduce the run. Not because the log was corrupt, not because the
    // simulation was non-deterministic, and not because anything reported an error — but because
    // the log was never complete. Every command in it replayed perfectly; the motion that never
    // became a command is simply gone.
    CY_CHECK_NE(replayed.hash(), live_hash);

    // And the shape of the divergence is the diagnosis: X — the axis that went through the stream —
    // is identical, and Y is not. A desync report months later would show exactly this, in a game
    // with ten thousand fields, and the search would start from nothing.
    for (u32 index = 0; index < MiniWorld::kEntities; ++index) {
        CY_CHECK_EQ(replayed.x[index], world.x[index]);
        CY_CHECK_NE(replayed.y[index], world.y[index]);
    }
}

CY_TEST_CASE(
    "gameplay: the two runs are otherwise identical, so the bypass is the only difference") {
    // THE CONTROL. Without it, the previous case proves only that two different programs produce
    // two different answers. This one runs the conforming shape with the *same* X intent as the
    // bypassing run — Y left at zero — and shows that its replay still matches. The command path is
    // therefore not what broke; the read around it is.
    Simulation conforming;
    CY_REQUIRE(conforming.build());
    MiniWorld world;

    for (u64 tick = 1; tick <= kTicks; ++tick) {
        for (u32 index = 0; index < MiniWorld::kEntities; ++index) {
            const Command command =
                conforming.move_command(index, SideChannel::dx_at(tick + index), 0);
            CY_REQUIRE(conforming.fixture.commands.producer(conforming.producer)
                           .record(command)
                           .has_value());
        }
        conforming.fixture.commands.commit(conforming.fixture.context(), tick);
        for (u32 index = 0; index < conforming.fixture.commands.committed_count(); ++index) {
            apply(world, conforming.fixture.commands.committed(index));
        }
    }

    MiniWorld replayed;
    for (u32 index = 0; index < conforming.fixture.commands.log().size(); ++index) {
        apply(replayed, conforming.fixture.commands.log().at(index));
    }
    CY_CHECK_EQ(replayed.hash(), world.hash());
}

CY_TEST_CASE("gameplay: a replay is a control source, indistinguishable from the player") {
    // `gameplay-framework` — "Replay playback SHALL be a control source producing commands, so
    // playback exercises the same simulation path as live play", and "the simulation SHALL NOT be
    // able to distinguish their origin".
    Simulation live;
    CY_REQUIRE(live.build());
    MiniWorld world;
    for (u64 tick = 1; tick <= 8; ++tick) {
        const Command command = live.move_command(0, static_cast<i32>(tick), 1);
        CY_REQUIRE(live.fixture.commands.producer(live.producer).record(command).has_value());
        live.fixture.commands.commit(live.fixture.context(), tick);
        for (u32 index = 0; index < live.fixture.commands.committed_count(); ++index) {
            apply(world, live.fixture.commands.committed(index));
        }
    }

    // Playback: a *second session*, with its own registry and stream, driven by a source of kind
    // `Replay` submitting the recorded commands through the ordinary producer path.
    Simulation playback;
    CY_REQUIRE(playback.build());
    auto replay_source = playback.fixture.control.create_source(
        ControlSourceKind::Replay, playback.participant, cy::Name::intern("replay"));
    CY_REQUIRE(replay_source.has_value());
    for (u32 index = 0; index < MiniWorld::kEntities; ++index) {
        CY_REQUIRE(playback.fixture.control
                       .bind_entity(*replay_source, channels::movement(), entity(index))
                       .has_value());
    }

    MiniWorld played;
    for (u64 tick = 1; tick <= 8; ++tick) {
        for (u32 index = 0; index < live.fixture.commands.log().size(); ++index) {
            const Command& recorded = live.fixture.commands.log().at(index);
            if (recorded.tick != tick) {
                continue;
            }
            Command command = recorded;
            command.source = *replay_source;
            command.provenance = Provenance{ControlSourceKind::Replay, 1};
            CY_REQUIRE(
                playback.fixture.commands.producer(playback.producer).record(command).has_value());
        }
        playback.fixture.commands.commit(playback.fixture.context(), tick);
        for (u32 index = 0; index < playback.fixture.commands.committed_count(); ++index) {
            apply(played, playback.fixture.commands.committed(index));
        }
    }

    CY_CHECK_EQ(played.hash(), world.hash());
    // And the two logs hash the same, because the hash excludes provenance — see CommandLog::hash.
    // A replay whose log differed from the run it reproduces would be a replay nobody could verify.
    CY_CHECK_EQ(playback.fixture.commands.log().hash(), live.fixture.commands.log().hash());
}
