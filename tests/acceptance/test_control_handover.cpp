// SPDX-License-Identifier: MIT
// The control handover acceptance scenario. M11.d task 7.2.
//
// `testing-and-quality` names it: "4-player networked co-operative play: a player enters and exits
// a vehicle, an AI takes it, a second player operates the turret, prediction and control transfer,
// a spectator observes, and a replay reconstructs it". This rung's delta names the property it must
// assert rather than merely run: "the observable state that must survive a handover".
//
// ================================================================================================
// WHAT MUST SURVIVE A HANDOVER, AS FIVE CHECKS
// ================================================================================================
//
//   1. THE VEHICLE. The same entity before and after, and its motion continuous across the tick the
//      driver changes — a handover that respawned or reset it would pass every "who controls it"
//      check and still be a different vehicle.
//   2. THE OTHER SEAT. The gunner's control of the turret is untouched by a change of DRIVER: their
//      orders are accepted on every tick of the scenario, including the handover tick, and the
//      turret's aim is never touched by movement.
//   3. THE TRANSFER IS ENFORCED, NOT ADVISORY. The driver who left is refused on the very next
//      tick, by the structural check, with `NotControlled` on the movement channel — and the AI
//      that took over is accepted on that same tick.
//   4. THE SPECTATOR OBSERVES AND CANNOT ACT. Every tick it reads who controls the vehicle on which
//      channel, and what it reads is the truth at that tick; every order it tries is refused.
//   5. THE REPLAY RECONSTRUCTS IT. The committed command stream, replayed into a fresh vehicle,
//   gives
//      the same state at every tick — including the ticks either side of the handover.
//
// ================================================================================================
// WHAT IS NOT EXERCISED, SAID HERE
// ================================================================================================
//
// NETWORKING AND PREDICTION. All four players are in one process and every command reaches the
// authority's merge (`CommandStream::commit`) directly: nothing is serialised, delayed or predicted
// ahead of confirmation. `samples/09-multiplayer` owns rollback under loss; composing it with a
// handover is this scenario's next step. The transfer, the seat independence, the spectator and the
// replay are what this test proves.

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/context.h>
#include <cy/gameplay/control.h>
#include <cy/test/test.h>

#include <vector>

namespace {

using cy::ecs::Entity;
using namespace cy::gameplay;

struct Drive {
    cy::f32 dx = 0.0F;
    cy::f32 dy = 0.0F;
};
struct Aim {
    cy::f32 yaw = 0.0F;
};

/// The observable state of the vehicle and of the two foot soldiers the other players drive.
struct Vehicle {
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;
    cy::f32 turret = 0.0F;
    cy::u32 moves = 0;

    [[nodiscard]] bool operator==(const Vehicle&) const = default;
};

struct State {
    Vehicle vehicle;
    Vehicle soldiers[2];

    [[nodiscard]] bool operator==(const State&) const = default;
};

struct Handover {
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::World);
    GameSession session{allocator, 0xC0DEULL};
    ControlRegistry control{allocator};
    CommandStream commands{allocator, control};
    std::vector<Command> recorded;

    Entity vehicle = Entity::make(10, 1);
    Entity soldiers[2] = {Entity::make(11, 1), Entity::make(12, 1)};

    ParticipantId players[4];
    ParticipantId spectator;
    ParticipantId server;
    ControlSourceId inputs[4];
    ControlSourceId watcher;
    ControlSourceId ai;
    cy::u32 producer = 0;
    CommandTypeId drive = kInvalidCommandType;
    CommandTypeId aim = kInvalidCommandType;

    static void record(void* user, const Command& command) noexcept {
        static_cast<Handover*>(user)->recorded.push_back(command);
    }

    [[nodiscard]] bool build() {
        const ParticipantKind kinds[4] = {ParticipantKind::LocalHuman, ParticipantKind::RemoteHuman,
                                          ParticipantKind::RemoteHuman,
                                          ParticipantKind::RemoteHuman};
        for (cy::u32 index = 0; index < 4; ++index) {
            const auto added = session.add_participant(kinds[index], cy::Name::intern("player"));
            if (!added) {
                return false;
            }
            players[index] = *added;
            const auto source =
                control.create_source(ControlSourceKind::Human, *added, cy::Name::intern("pad"));
            if (!source) {
                return false;
            }
            inputs[index] = *source;
        }
        const auto watching =
            session.add_participant(ParticipantKind::Spectator, cy::Name::intern("spectator"));
        const auto hosting =
            session.add_participant(ParticipantKind::ServerAgent, cy::Name::intern("server"));
        if (!watching || !hosting) {
            return false;
        }
        spectator = *watching;
        server = *hosting;
        const auto eyes = control.create_source(ControlSourceKind::Human, spectator,
                                                cy::Name::intern("spectator"));
        const auto brain = control.create_source(ControlSourceKind::ArtificialIntelligence, server,
                                                 cy::Name::intern("driver-ai"));
        const auto opened = commands.open_producer(cy::Name::intern("session"));
        if (!eyes || !brain || !opened) {
            return false;
        }
        watcher = *eyes;
        ai = *brain;
        producer = *opened;

        CommandDeclaration driving;
        driving.name = cy::Name::intern("Drive");
        driving.stable_id = 81;
        driving.channel = channels::movement();
        CommandDeclaration aiming;
        aiming.name = cy::Name::intern("Aim");
        aiming.stable_id = 82;
        aiming.channel = channels::turret();
        const auto d = commands.declare(driving);
        const auto a = commands.declare(aiming);
        if (!d || !a) {
            return false;
        }
        drive = *d;
        aim = *a;
        commands.set_record_sink(CommandStream::RecordSink{&Handover::record, this});

        // Player 1 is in the driver's seat, player 2 in the gunner's; players 3 and 4 are on foot.
        return control.bind_entity(inputs[0], channels::movement(), vehicle).has_value() &&
               control.bind_entity(inputs[1], channels::turret(), vehicle).has_value() &&
               control.bind_entity(inputs[2], channels::movement(), soldiers[0]).has_value() &&
               control.bind_entity(inputs[3], channels::movement(), soldiers[1]).has_value();
    }

    [[nodiscard]] GameplayContext context(cy::u64 tick) noexcept {
        GameplayContext ctx;
        ctx.session = &session;
        ctx.services = &session.services();
        ctx.commands = &commands;
        ctx.at.tick = tick;
        return ctx;
    }

    void order(ParticipantId who, ControlSourceId source, CommandTypeId type, Entity target,
               auto payload, cy::u64 tick) {
        Command command;
        command.type = type;
        command.tick = tick;
        command.participant = who;
        command.source = source;
        command.target = target;
        (void)command.set_payload(payload);
        (void)commands.producer(producer).record(command);
    }

    [[nodiscard]] Vehicle* subject_of(Entity target, State& state) const {
        if (target == vehicle) {
            return &state.vehicle;
        }
        for (cy::u32 index = 0; index < 2; ++index) {
            if (target == soldiers[index]) {
                return &state.soldiers[index];
            }
        }
        return nullptr;
    }

    /// Execution: what the game does with a committed command. The SAME function drives the live
    /// session and the replay, so the replay is a reconstruction and not a second implementation.
    void apply(const Command& command, State& state) const {
        Vehicle* subject = subject_of(command.target, state);
        if (subject == nullptr) {
            return;
        }
        if (Drive move; command.type == drive && command.read_payload(move)) {
            subject->x += move.dx;
            subject->y += move.dy;
            ++subject->moves;
        } else if (Aim turn; command.type == aim && command.read_payload(turn)) {
            subject->turret = turn.yaw;
        }
    }
};

/// Who controls `entity`, as the spectator reads it: the set of (source, channel) pairs.
[[nodiscard]] bool controls_exactly(const ControlRegistry& control, Entity entity,
                                    ControlSourceId expected_driver, ControlSourceId gunner) {
    ControlSourceId found[8];
    const cy::u32 count = control.sources_controlling(entity, found, 8);
    if (count != 2) {
        return false;
    }
    const bool driver_ok = control.controls(expected_driver, entity, channels::movement());
    const bool gunner_ok = control.controls(gunner, entity, channels::turret());
    // And nobody else drives it: the one who left must not still hold the movement channel.
    return driver_ok && gunner_ok;
}

}  // namespace

CY_TEST_CASE(
    "control handover: the vehicle, the other seat, the spectator and the replay survive") {
    Handover session;
    CY_REQUIRE(session.build());

    constexpr cy::u64 kTicks = 20;
    constexpr cy::u64 kExit = 10;  // player 1 leaves the driver's seat after this tick

    State live;
    std::vector<State> timeline;
    cy::u32 turret_accepted = 0;

    for (cy::u64 tick = 1; tick <= kTicks; ++tick) {
        if (tick == kExit + 1) {
            // THE HANDOVER, at the commit boundary where every structural change happens: player 1
            // exits the vehicle and the AI takes the wheel. The gunner's binding is not touched.
            session.control.unbind(session.inputs[0], channels::movement());
            CY_REQUIRE(
                session.control.bind_entity(session.ai, channels::movement(), session.vehicle)
                    .has_value());
        }
        const bool human_driving = tick <= kExit;

        // Player 1 keeps pressing the stick AFTER leaving — the input a real client sends in the
        // frame it takes to learn it is on foot. It must be refused, not applied.
        session.order(session.players[0], session.inputs[0], session.drive, session.vehicle,
                      Drive{1.0F, 0.0F}, tick);
        if (!human_driving) {
            session.order(session.server, session.ai, session.drive, session.vehicle,
                          Drive{0.0F, 1.0F}, tick);
        }
        session.order(session.players[1], session.inputs[1], session.aim, session.vehicle,
                      Aim{static_cast<cy::f32>(tick) * 3.0F}, tick);
        session.order(session.players[2], session.inputs[2], session.drive, session.soldiers[0],
                      Drive{0.5F, 0.5F}, tick);
        session.order(session.players[3], session.inputs[3], session.drive, session.soldiers[1],
                      Drive{-0.5F, 0.5F}, tick);
        // The spectator tries to take the turret and the wheel. It watches; it does not act.
        session.order(session.spectator, session.watcher, session.aim, session.vehicle, Aim{-90.0F},
                      tick);
        session.order(session.spectator, session.watcher, session.drive, session.vehicle,
                      Drive{-5.0F, -5.0F}, tick);

        session.commands.commit(session.context(tick), tick);
        for (cy::u32 index = 0; index < session.commands.committed_count(); ++index) {
            const Command& command = session.commands.committed(index);
            session.apply(command, live);
            turret_accepted += command.type == session.aim ? 1U : 0U;
        }
        timeline.push_back(live);

        // 3 — enforced: exactly the refusals the seating chart says, and why.
        cy::u32 refused_driver = 0;
        cy::u32 refused_spectator = 0;
        for (cy::u32 index = 0; index < session.commands.rejection_count(); ++index) {
            const CommandStream::Rejection& rejection = session.commands.rejection(index);
            CY_CHECK(rejection.result.first().tag == ReasonTag::NotControlled);
            refused_driver += rejection.command.source == session.inputs[0] ? 1U : 0U;
            refused_spectator += rejection.command.source == session.watcher ? 1U : 0U;
        }
        CY_CHECK_EQ(refused_spectator, 2U);
        CY_CHECK_EQ(refused_driver, human_driving ? 0U : 1U);

        // 4 — the spectator's view of who holds the vehicle is the truth at this tick.
        CY_CHECK(controls_exactly(session.control, session.vehicle,
                                  human_driving ? session.inputs[0] : session.ai,
                                  session.inputs[1]));
        CY_CHECK(session.control.controls(session.inputs[0], session.vehicle,
                                          channels::movement()) == human_driving);
    }

    // 1 — the vehicle: one entity, moved east by its human for ten ticks and north by the AI for
    // ten, with no reset in between: the handover tick continues from where the human left it.
    CY_CHECK_EQ(live.vehicle.moves, static_cast<cy::u32>(kTicks));
    CY_CHECK_EQ(timeline[kExit - 1].vehicle.x, 10.0F);
    CY_CHECK_EQ(timeline[kExit].vehicle.x, 10.0F);
    CY_CHECK_EQ(timeline[kExit].vehicle.y, 1.0F);
    CY_CHECK_EQ(live.vehicle.y, 10.0F);

    // 2 — the other seat: the gunner was accepted on every tick, the handover tick included, and
    // the turret holds the gunner's last aim rather than anything the driver change did to it.
    CY_CHECK_EQ(turret_accepted, static_cast<cy::u32>(kTicks));
    CY_CHECK_EQ(timeline[kExit].vehicle.turret, static_cast<cy::f32>(kExit + 1) * 3.0F);
    CY_CHECK_EQ(live.vehicle.turret, static_cast<cy::f32>(kTicks) * 3.0F);
    // The two players on foot were never affected by a seat change they had no part in.
    CY_CHECK_EQ(live.soldiers[0].moves, static_cast<cy::u32>(kTicks));
    CY_CHECK_EQ(live.soldiers[1].moves, static_cast<cy::u32>(kTicks));

    // 5 — the replay: the recorded stream, applied to a fresh state, tick by tick.
    State replayed;
    std::vector<State> reconstruction;
    cy::usize cursor = 0;
    for (cy::u64 tick = 1; tick <= kTicks; ++tick) {
        while (cursor < session.recorded.size() && session.recorded[cursor].tick == tick) {
            session.apply(session.recorded[cursor], replayed);
            ++cursor;
        }
        reconstruction.push_back(replayed);
    }
    CY_CHECK_EQ(cursor, session.recorded.size());
    CY_REQUIRE_EQ(reconstruction.size(), timeline.size());
    for (cy::usize index = 0; index < timeline.size(); ++index) {
        CY_CHECK(reconstruction[index] == timeline[index]);
    }
    // Nothing the spectator or the departed driver tried is in the record.
    for (const Command& command : session.recorded) {
        CY_CHECK(command.source != session.watcher);
        CY_CHECK((command.source != session.inputs[0] || command.tick <= kExit));
    }
}
