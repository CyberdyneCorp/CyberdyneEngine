// SPDX-License-Identifier: MIT
// The strategy stress acceptance scenario. M11.d task 7.2.
//
// `testing-and-quality` — "Performance benchmarks": the suite SHALL include acceptance scenarios
// "exercising the architecture rather than a favourable case", and "the strategy scenario SHALL be
// treated as the primary architectural test, since a single-character scenario does not distinguish
// a data-oriented gameplay framework from an object-oriented one". The scenario it names:
//
//     8 participants, 4 teams, 100 000 units with 20 000 moving, 5 000 agent groups, 1 000
//     structures, hundreds of commands per frame, world streaming, network authority, and replay
//     recording
//
// and the property it is named for, from this rung's own delta: "framework overhead SHALL be
// measured and compared against a committed threshold, and a regression SHALL fail the suite".
//
// ================================================================================================
// WHAT A TICK IS HERE, AND WHICH HALF IS "THE FRAMEWORK"
// ================================================================================================
//
// THE FRAMEWORK is what `gameplay-framework` puts between a player's intent and the simulation:
// each participant's group orders routed to their members through the control registry, every
// member command validated STRUCTURALLY (the participant exists, its source controls the unit on
// the command channel) and committed in a deterministic merge, the committed stream handed to the
// replay-recording seam, and each participant's ownership query answered from the derived index.
//
// THE SIMULATION is what a strategy game does with the result: the ordered velocities applied, the
// moving units integrated through an ECS query, and every unit binned into a spatial grid and each
// moving unit's nearest hostile found in its cell — the target-acquisition pass a real RTS runs
// every tick over its whole population.
//
// The reported number is `framework / (framework + simulation)`, measured over the same ticks on
// the same thread. It is a RATIO of two measurements taken side by side, so a loaded machine slows
// both halves and moves it far less than it moves either — which is what lets a CTest entry defend
// it at all. It is not a wall-clock budget, and it is not asserted as one.
//
// ================================================================================================
// WHAT THIS SCENARIO DOES NOT EXERCISE, SAID HERE RATHER THAN IMPLIED
// ================================================================================================
//
//   * WORLD STREAMING. The 100 000 units live in one resident world; no cell loads or unloads
//     during the measured ticks. `world-partition-and-streaming` has its own suites; composing them
//     into this tick is the scenario's next step, not something this file claims.
//   * NETWORK AUTHORITY. Every participant is local and the merge is the authority's merge — the
//     same `CommandStream::commit` a host runs — but nothing is serialised or sent. The replay seam
//     IS exercised: every committed command reaches the record sink, and the test requires it.
//
// A reader who needs those two measured must not read this test's green as that measurement.
//
// ================================================================================================
// THE FINDING THIS SCENARIO PRODUCED THE FIRST TIME IT RAN
// ================================================================================================
//
// It could not be built. `ControlRegistry` refused a 65th group — `kMaxGroups` was 64 against the
// scenario's 5 000 — and `controls()`, the check validation runs on every member command, walked
// every binding and, for each group binding, searched the group LIST for the group and then its
// member list: at this scale, millions of comparisons per command and thousands of commands per
// tick. The registry now indexes bindings and memberships (see control.h), and
// `src/gameplay/tests/test_control.cpp` holds the regression at the registry's own level.

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/type_info.h>
#include <cy/ecs/query.h>
#include <cy/ecs/world.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/context.h>
#include <cy/gameplay/control.h>
#include <cy/gameplay/indexes.h>
#include <cy/test/test.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace {

using cy::ecs::ComponentTypeId;
using cy::ecs::Entity;
using namespace cy::gameplay;

// --- The scenario's own numbers, as the specification writes them --------------------------------

constexpr cy::u32 kParticipants = 8;
constexpr cy::u32 kTeams = 4;
constexpr cy::u32 kUnits = 100'000;
constexpr cy::u32 kMoving = 20'000;
constexpr cy::u32 kGroups = 5'000;
constexpr cy::u32 kStructures = 1'000;
constexpr cy::u32 kGroupSize = kUnits / kGroups;  // 20
/// Group orders per participant per tick. Eight participants give 320 orders a tick — "hundreds of
/// commands per frame" — which the routing turns into 6 400 member commands, each validated.
constexpr cy::u32 kOrdersPerParticipant = 40;
constexpr cy::u32 kWarmupTicks = 3;
constexpr cy::u32 kMeasuredTicks = 20;

/// THE COMMITTED THRESHOLD. The framework's share of a strategy tick may not exceed this.
///
/// MEASURED, and recorded with what it was measured against, because the number is only as
/// meaningful as the tick it divides by. On the development host (i9-12900K, Development
/// configuration, load average 20+ on 24 threads while five agents built) the share was:
///
///   before the control registry was indexed    94 %   (50.7 ms framework, 3.2 ms simulation)
///   after                                      22–34 % (0.5–1.2 ms framework, 1.9–2.3 ms sim.)
///
/// — about 150 ns of routing, structural validation, commit and recording per member command, for
/// 6 400 of them a tick. The ceiling is 50 %: it passes the spread above on a loaded machine and
/// fails the architectural regression this scenario exists to catch, which moved the share by a
/// factor of three and the framework's cost by a factor of a hundred.
///
/// IT IS A REGRESSION GUARD, NOT THE CLAIM THAT A THIRD IS "SMALL". This tick's simulation is a
/// modest one — movement and target acquisition, no pathfinding, no combat resolution, no streaming
/// — so the same framework cost is a smaller share of a real strategy tick. `gameplay-framework`'s
/// "small, reported fraction" is judged against that tick, and this scenario does not have it yet;
/// tasks.md 7.2 records the gap rather than this constant hiding it. A first draft of this file set
/// the ceiling at 15 % BEFORE anything was measured; the measurement replaced the guess, and the
/// guess is recorded here so nobody mistakes 50 % for a loosened check. Changing it is
/// `testing-and-quality`'s "Intentional trade-off": in the same change, with the reason.
constexpr double kFrameworkShareCeiling = 0.50;

struct Position {
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;
};
struct Velocity {
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;
};
struct Allegiance {
    cy::u32 team = 0;
};

template <class T>
const cy::reflect::TypeInfo& descriptor(const char* name, cy::u32 id) noexcept {
    static cy::reflect::TypeInfo info;
    info.name = name;
    info.id = cy::reflect::TypeId(id);
    info.size = static_cast<cy::u32>(sizeof(T));
    info.alignment = static_cast<cy::u32>(alignof(T));
    info.trivially_relocatable = true;
    return info;
}

struct Order {
    cy::f32 vx = 0.0F;
    cy::f32 vy = 0.0F;
};

/// The replay-recording seam: every committed command, in commit order.
struct Recorder {
    cy::u64 recorded = 0;
    cy::u64 fold = 0;

    static void sink(void* user, const Command& command) noexcept {
        auto* self = static_cast<Recorder*>(user);
        ++self->recorded;
        self->fold = (self->fold * 1099511628211ULL) ^ command.target.bits() ^ command.tick;
    }
};

/// A spatial grid over the battlefield, rebuilt every tick: the target-acquisition pass.
struct Grid {
    static constexpr cy::u32 kCells = 64;
    static constexpr cy::f32 kWorld = 4096.0F;
    std::vector<cy::u32> starts = std::vector<cy::u32>((kCells * kCells) + 1, 0);
    std::vector<cy::u32> members;

    [[nodiscard]] static cy::u32 cell_of(const Position& p) noexcept {
        const auto clamp = [](cy::f32 v) {
            const cy::i32 cell = std::clamp(static_cast<cy::i32>(v / (kWorld / kCells)), 0,
                                            static_cast<cy::i32>(kCells) - 1);
            return static_cast<cy::u32>(cell);
        };
        return (clamp(p.y) * kCells) + clamp(p.x);
    }
};

struct Scenario {
    cy::Allocator& allocator = cy::system_allocator(cy::MemoryDomain::World);
    cy::ecs::World world{allocator};
    GameSession session{allocator, 0x57A7ULL};
    ControlRegistry control{allocator};
    CommandStream commands{allocator, control};
    GameplayIndexes indexes{allocator};
    Recorder recorder;

    ComponentTypeId position = cy::ecs::kInvalidComponent;
    ComponentTypeId velocity = cy::ecs::kInvalidComponent;
    ComponentTypeId allegiance = cy::ecs::kInvalidComponent;

    ParticipantId participants[kParticipants];
    ControlSourceId sources[kParticipants];
    cy::u32 producers[kParticipants] = {};
    std::vector<GroupId> groups;
    cy::Array<Entity> units{allocator};
    cy::Array<Entity> structures{allocator};
    CommandTypeId order = kInvalidCommandType;

    std::vector<Position> positions;
    std::vector<cy::u32> teams;
    Grid grid;
    cy::Array<Entity> owned{allocator};
    cy::Array<Entity> excluded{allocator};

    bool built = false;

    Scenario() { built = build(); }

    [[nodiscard]] bool build() {
        if (!world.initialize()) {
            return false;
        }
        const auto p = world.components().register_reflected(
            descriptor<Position>("cy::acceptance::Position", 9101));
        const auto v = world.components().register_reflected(
            descriptor<Velocity>("cy::acceptance::Velocity", 9102));
        const auto a = world.components().register_reflected(
            descriptor<Allegiance>("cy::acceptance::Allegiance", 9103));
        if (!p || !v || !a) {
            return false;
        }
        position = *p;
        velocity = *v;
        allegiance = *a;

        // Every unit can move; the first kMoving are the ones the orders keep moving.
        const ComponentTypeId unit_set[] = {position, velocity, allegiance};
        const ComponentTypeId structure_set[] = {position, allegiance};
        if (!world.create_many(kUnits, cy::Span<const ComponentTypeId>(unit_set, 3), units) ||
            !world.create_many(kStructures, cy::Span<const ComponentTypeId>(structure_set, 2),
                               structures)) {
            return false;
        }

        for (cy::u32 index = 0; index < kParticipants; ++index) {
            const auto added = session.add_participant(ParticipantKind::LocalHuman,
                                                       cy::Name::intern("player"), index % kTeams);
            if (!added) {
                return false;
            }
            participants[index] = *added;
            const auto source =
                control.create_source(ControlSourceKind::Human, *added, cy::Name::intern("input"));
            const auto producer = commands.open_producer(cy::Name::intern("player"));
            if (!source || !producer) {
                return false;
            }
            sources[index] = *source;
            producers[index] = *producer;
        }

        // Place every unit and structure, give it an owner and a team, and index the ownership.
        const auto place = [&](Entity entity, cy::u32 serial, cy::u32 owner) {
            auto* at = world.get_mut<Position>(entity, position);
            auto* side = world.get_mut<Allegiance>(entity, allegiance);
            if (at == nullptr || side == nullptr) {
                return false;
            }
            at->x = static_cast<cy::f32>((serial * 7919U) % 4096U);
            at->y = static_cast<cy::f32>((serial * 104729U) % 4096U);
            side->team = owner % kTeams;
            return indexes.on_owner_changed(entity, participants[owner]).has_value();
        };
        for (cy::u32 index = 0; index < kUnits; ++index) {
            if (!place(units[index], index, (index / kGroupSize) % kParticipants)) {
                return false;
            }
        }
        for (cy::u32 index = 0; index < kStructures; ++index) {
            if (!place(structures[index], kUnits + index, index % kParticipants)) {
                return false;
            }
        }

        // 5 000 groups of 20, each bound ONCE to its owner's source on the command channel — the
        // shape `gameplay-framework` requires rather than one binding per unit.
        groups.reserve(kGroups);
        for (cy::u32 group = 0; group < kGroups; ++group) {
            const auto created = control.create_group(cy::Name::intern("squad"));
            if (!created) {
                std::fprintf(stderr, "strategy stress: group %u refused: %s\n", group,
                             created.error().message);
                return false;
            }
            for (cy::u32 member = 0; member < kGroupSize; ++member) {
                if (!control.add_to_group(*created, units[(group * kGroupSize) + member])) {
                    return false;
                }
            }
            if (!control.bind_group(sources[group % kParticipants], channels::command(),
                                    *created)) {
                return false;
            }
            groups.push_back(*created);
        }

        CommandDeclaration declaration;
        declaration.name = cy::Name::intern("MoveOrder");
        declaration.stable_id = 71;
        declaration.channel = channels::command();
        const auto declared = commands.declare(declaration);
        if (!declared) {
            return false;
        }
        order = *declared;
        commands.set_record_sink(CommandStream::RecordSink{&Recorder::sink, &recorder});

        positions.resize(kUnits + kStructures);
        teams.resize(kUnits + kStructures);
        grid.members.resize(kUnits + kStructures);
        return owned.resize(kUnits).has_value();
    }

    [[nodiscard]] GameplayContext context(cy::u64 tick) noexcept {
        GameplayContext ctx;
        ctx.world = &world;
        ctx.session = &session;
        ctx.services = &session.services();
        ctx.commands = &commands;
        ctx.at.tick = tick;
        return ctx;
    }

    /// THE FRAMEWORK: route, validate, commit, record, and answer ownership. Returns the number of
    /// member commands committed.
    [[nodiscard]] cy::u32 framework(cy::u64 tick) {
        for (cy::u32 participant = 0; participant < kParticipants; ++participant) {
            for (cy::u32 issued = 0; issued < kOrdersPerParticipant; ++issued) {
                // A participant orders only its OWN groups that hold moving units — group g belongs
                // to participant g % 8, and the first kMoving / 20 groups are the moving ones — and
                // rotates through them, so each tick orders forty different squads.
                const cy::u32 own_moving = kMoving / kGroupSize / kParticipants;
                const cy::u32 pick =
                    ((static_cast<cy::u32>(tick) * kOrdersPerParticipant) + issued) % own_moving;
                const cy::u32 group = (pick * kParticipants) + participant;
                Command prototype;
                prototype.type = order;
                prototype.tick = tick;
                prototype.participant = participants[participant];
                prototype.source = sources[participant];
                prototype.group = groups[group];
                (void)prototype.set_payload(Order{static_cast<cy::f32>((tick % 7) + 1),
                                                  static_cast<cy::f32>((group % 5) + 1)});
                excluded.clear();
                if (!commands.route_to_group(prototype, producers[participant], excluded)) {
                    return 0;
                }
            }
        }
        commands.commit(context(tick), tick);
        for (const ParticipantId participant : participants) {
            (void)indexes.owned_by(participant, owned.data(), static_cast<cy::u32>(owned.size()));
        }
        return commands.committed_count();
    }

    /// THE SIMULATION: apply the orders, integrate the movers, bin everything, acquire targets.
    [[nodiscard]] cy::u64 simulation() {
        for (cy::u32 index = 0; index < commands.committed_count(); ++index) {
            const Command& command = commands.committed(index);
            Order intent;
            auto* heading = world.get_mut<Velocity>(command.target, velocity);
            if (heading != nullptr && command.read_payload(intent)) {
                heading->x = intent.vx;
                heading->y = intent.vy;
            }
        }

        cy::ecs::QueryDesc desc(allocator);
        (void)desc.write(position);
        (void)desc.read(velocity);
        (void)desc.read(allegiance);
        cy::ecs::Query query(world, std::move(desc));
        cy::u32 written = 0;
        (void)query.for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
            const cy::Span<Position> at = chunk.write<Position>(position);
            const cy::Span<const Velocity> heading = chunk.read<Velocity>(velocity);
            const cy::Span<const Allegiance> side = chunk.read<Allegiance>(allegiance);
            for (cy::u32 row = 0; row < chunk.count(); ++row) {
                at[row].x += heading[row].x * 0.05F;
                at[row].y += heading[row].y * 0.05F;
                positions[written] = at[row];
                teams[written] = side[row].team;
                ++written;
            }
        });
        for (const Entity structure : structures) {
            positions[written] = *world.get<Position>(structure, position);
            teams[written] = world.get<Allegiance>(structure, allegiance)->team;
            ++written;
        }

        // Bin: a counting sort into the grid, every unit and structure.
        std::ranges::fill(grid.starts, 0U);
        for (cy::u32 index = 0; index < written; ++index) {
            ++grid.starts[Grid::cell_of(positions[index]) + 1];
        }
        for (cy::u32 cell = 1; cell < grid.starts.size(); ++cell) {
            grid.starts[cell] += grid.starts[cell - 1];
        }
        std::vector<cy::u32> cursor(grid.starts.begin(), grid.starts.end() - 1);
        for (cy::u32 index = 0; index < written; ++index) {
            grid.members[cursor[Grid::cell_of(positions[index])]++] = index;
        }

        // Acquire: each of the first kMoving units finds its nearest hostile in its own cell.
        cy::u64 acquired = 0;
        for (cy::u32 index = 0; index < kMoving; ++index) {
            const cy::u32 cell = Grid::cell_of(positions[index]);
            cy::f32 best = 1.0e30F;
            cy::u32 target = 0xFFFFFFFFU;
            for (cy::u32 at = grid.starts[cell]; at < grid.starts[cell + 1]; ++at) {
                const cy::u32 other = grid.members[at];
                if (teams[other] == teams[index]) {
                    continue;
                }
                const cy::f32 dx = positions[other].x - positions[index].x;
                const cy::f32 dy = positions[other].y - positions[index].y;
                const cy::f32 distance = (dx * dx) + (dy * dy);
                if (distance < best) {
                    best = distance;
                    target = other;
                }
            }
            acquired += target != 0xFFFFFFFFU ? 1U : 0U;
        }
        return acquired;
    }
};

using Clock = std::chrono::steady_clock;

[[nodiscard]] cy::u64 elapsed_ns(Clock::time_point since) {
    return static_cast<cy::u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - since).count());
}

}  // namespace

CY_TEST_CASE("strategy stress: the framework is a small, measured fraction of a strategy tick") {
    Scenario scenario;
    CY_REQUIRE(scenario.built);
    if (!scenario.built) {
        return;  // doctest's REQUIRE does not unwind in this build; nothing below is meaningful
    }

    // The scenario is what the specification says it is, checked rather than assumed — a fixture
    // that quietly built 64 groups would measure a different architecture.
    CY_CHECK_EQ(scenario.session.participant_count(), kParticipants);
    CY_CHECK_EQ(scenario.groups.size(), kGroups);
    CY_CHECK_EQ(scenario.units.size(), kUnits);
    CY_CHECK_EQ(scenario.structures.size(), kStructures);

    cy::u64 framework_ns = 0;
    cy::u64 simulation_ns = 0;
    cy::u64 committed = 0;
    cy::u64 acquired = 0;
    for (cy::u32 tick = 1; tick <= kWarmupTicks + kMeasuredTicks; ++tick) {
        const Clock::time_point framework_began = Clock::now();
        const cy::u32 this_tick = scenario.framework(tick);
        const cy::u64 framework_took = elapsed_ns(framework_began);

        const Clock::time_point simulation_began = Clock::now();
        const cy::u64 targets = scenario.simulation();
        const cy::u64 simulation_took = elapsed_ns(simulation_began);

        // Every order reached every member and every member command was ACCEPTED: a fast tick made
        // of rejections would measure the rejection path and be about nothing.
        CY_REQUIRE_EQ(this_tick, kParticipants * kOrdersPerParticipant * kGroupSize);
        CY_REQUIRE_EQ(scenario.commands.rejection_count(), 0U);
        scenario.commands.log().clear();
        if (tick > kWarmupTicks) {
            framework_ns += framework_took;
            simulation_ns += simulation_took;
            committed += this_tick;
            acquired += targets;
        }
    }

    // The replay seam saw every committed command, in every tick — warm-up included.
    CY_CHECK(scenario.recorder.recorded >= committed);
    CY_CHECK(acquired > 0U);

    const double share =
        static_cast<double>(framework_ns) / static_cast<double>(framework_ns + simulation_ns);
    std::printf(
        "strategy stress: %u participants, %u teams, %u units (%u moving), %u groups, "
        "%u structures\n",
        kParticipants, kTeams, kUnits, kMoving, kGroups, kStructures);
    std::printf(
        "strategy stress: %llu member commands over %u ticks (%llu a tick), "
        "%llu recorded\n",
        static_cast<unsigned long long>(committed), kMeasuredTicks,
        static_cast<unsigned long long>(committed / kMeasuredTicks),
        static_cast<unsigned long long>(scenario.recorder.recorded));
    std::printf(
        "strategy stress: framework %.3f ms/tick, simulation %.3f ms/tick, "
        "framework share %.2f%% (ceiling %.0f%%)\n",
        static_cast<double>(framework_ns) / 1.0e6 / kMeasuredTicks,
        static_cast<double>(simulation_ns) / 1.0e6 / kMeasuredTicks, share * 100.0,
        kFrameworkShareCeiling * 100.0);
    std::printf(
        "strategy stress: NOT EXERCISED world streaming, network authority — see the "
        "header of tests/acceptance/test_strategy_stress.cpp\n");
    CY_CHECK(share <= kFrameworkShareCeiling);
}
