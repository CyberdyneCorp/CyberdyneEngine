// SPDX-License-Identifier: MIT
// cy_headless_server — the headless server acceptance scenario's program. M11.d task 7.2.
//
// `testing-and-quality`: "A dedicated server with no renderer, GPU, audio, or interface, running
// 100 000 entities with AI, commands, and physics at a fixed simulation rate", and "WHEN the
// headless scenario runs THEN it SHALL execute with no rendering, audio, or interface code linked,
// and a dependency on any of them SHALL fail the build". `diagnostics-profiling-and-crash`: "WHEN a
// dedicated server runs THEN its diagnostics SHALL be available with no rendering or interface code
// present".
//
// ================================================================================================
// WHY THIS IS A PROGRAM OF ITS OWN
// ================================================================================================
//
// Until M11.d "headless" was a RUN-TIME choice of display server on the ordinary binary — `just
// run-headless` is `just run-sample --headless` — so the renderer, the audio backend and the
// interface were linked into it whatever it chose at startup, and the build claim had nothing to
// check. This target is the dedicated-server BUILD CONFIGURATION: it links the simulation and
// nothing that draws, plays or presents, and tests/acceptance/CMakeLists.txt refuses the configure
// if its link closure ever reaches one of them.
//
// ================================================================================================
// ONE TICK
// ================================================================================================
//
//   AI        `cy::ai::AiRuntime` thinks over every agent it is due to, tiered so that 100 000
//             agents cost a bounded number of thinks per tick, with no agent starved.
//   COMMANDS  every agent that decided to move becomes a command from the AI's control source,
//             addressed to the agent through its squad's ONE group binding, validated and
//             committed by the same `CommandStream` a game uses.
//   PHYSICS   each committed command sets its agent's body moving; the Jolt world steps at the
//             fixed rate with every entity a body in it.
//   OBSERVE   the tick is written to a trace: tick rate, work per tick and overruns, memory in
//             use, AI thinks and starvation, commands committed and refused, bodies and active
//             bodies, and a per-tick state hash for divergence — every one of them readable with
//             `just diagnose-trace`, a tool that does not link the engine.
//
// What it does NOT have, and prints: connections, replication and world streaming. A dedicated
// server has all three; this program has no transport and one resident world, so those counters
// would be zeros that looked like measurements.

#include <cy/ai/agent.h>
#include <cy/ai/runtime.h>
#include <cy/backends/physics/jolt/server.h>
#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/trace.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/context.h>
#include <cy/gameplay/control.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_behaviour.h>
#include <cy/servers/physics/server.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace cy;
using cy::ecs::Entity;

constexpr const char* kTag = "cy_headless_server";

struct Options {
    u32 entities = 100'000;
    u32 ticks = 90;
    u32 rate = 30;
    bool paced = true;
    std::string trace = "headless-server.cytrace";
};

[[nodiscard]] bool parse(int argc, char** argv, Options& out) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const bool has_value = index + 1 < argc;
        if (argument == "--entities" && has_value) {
            out.entities = static_cast<u32>(std::strtoul(argv[++index], nullptr, 10));
        } else if (argument == "--ticks" && has_value) {
            out.ticks = static_cast<u32>(std::strtoul(argv[++index], nullptr, 10));
        } else if (argument == "--rate" && has_value) {
            out.rate = static_cast<u32>(std::strtoul(argv[++index], nullptr, 10));
        } else if (argument == "--trace" && has_value) {
            out.trace = argv[++index];
        } else if (argument == "--unpaced") {
            out.paced = false;
        } else {
            std::fprintf(stderr,
                         "usage: %s [--entities N] [--ticks N] [--rate HZ] [--trace FILE] "
                         "[--unpaced]\n",
                         kTag);
            return false;
        }
    }
    return out.entities > 0 && out.rate > 0;
}

[[nodiscard]] int fail(const char* what, const Error& error) {
    std::fprintf(stderr, "%s: %s: %s\n", kTag, what, error.message);
    return 1;
}

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

// --- AI: one behaviour for every agent
// ------------------------------------------------------------

[[nodiscard]] graph::Literal name_literal(const char* value) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

/// root → sequence(condition "threat_near", task "reposition"): think, and when there is a reason,
/// move. The same shape the AI suites run, built here because a server owns its own content.
[[nodiscard]] Expected<graph::behaviour::BehaviourProgram, Error> behaviour(
    graph::NodeRegistry& registry, graph::DiagnosticSink& sink) {
    graph::Graph tree(allocator(), Name::intern("sentry"));
    const bool built =
        tree.add_node(1, Name::intern("ai.root")).has_value() &&
        tree.add_node(2, Name::intern("ai.sequence")).has_value() &&
        tree.add_node(3, Name::intern("ai.condition")).has_value() &&
        tree.set_property(3, Name::intern("task"), name_literal("threat_near")).has_value() &&
        tree.add_node(4, Name::intern("ai.task")).has_value() &&
        tree.set_property(4, Name::intern("task"), name_literal("reposition")).has_value() &&
        tree.connect(2, Name::intern("node"), 1, Name::intern("child")).has_value() &&
        tree.connect(3, Name::intern("node"), 2, Name::intern("children")).has_value() &&
        tree.connect(4, Name::intern("node"), 2, Name::intern("children")).has_value();
    if (!built) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "the behaviour did not build", 0});
    }
    tree.resolve(registry);
    return graph::behaviour::compile_behaviour(tree, registry, sink);
}

/// Tiered as a crowd is: 1 % Full, 6 % Reduced, 30 % Minimal and the rest Statistical, so 100 000
/// agents cost a bounded number of thinks a tick and every one of them thinks within its interval.
[[nodiscard]] ai::AiTier tier_for(u32 index) noexcept {
    const u32 band = index % 100U;
    if (band < 1U) {
        return ai::AiTier::Full;
    }
    if (band < 7U) {
        return ai::AiTier::Reduced;
    }
    if (band < 37U) {
        return ai::AiTier::Minimal;
    }
    return ai::AiTier::Statistical;
}

/// One decision in this many finds a reason to move. With ~2 700 thinks a tick that is ~270 orders
/// a tick — 8 000 a second at 30 Hz — which is a busy strategy server, not an idle one.
constexpr u32 kOrderEvery = 10;
/// An order moves a unit for this many ticks, then it stops and its body can sleep again: the
/// active set tracks the units under orders rather than every unit ever ordered.
constexpr u32 kOrderTicks = 15;

/// The world as the behaviour sees it. Deterministic: a threat is "near" on a fixed rotation, so
/// two runs make the same decisions and the state hash can mean something.
class Host final : public graph::behaviour::BehaviourHost {
public:
    graph::behaviour::BtStatus run_task(Name /*task*/, f32 /*dt*/) override {
        ++tasks;
        return graph::behaviour::BtStatus::Success;
    }
    [[nodiscard]] bool test_condition(Name /*condition*/) override {
        return ((++asked) % kOrderEvery) == 0U;
    }
    [[nodiscard]] f32 score(Name /*task*/) override { return 0.5F; }

    u64 tasks = 0;
    u64 asked = 0;
};

// --- Observability: the counters a dedicated server owes
// ------------------------------------------

struct Counters {
    diag::CategoryId category = diag::register_category("server");
    diag::NameId tick_rate = diag::register_name("server.tick_rate_hz");
    diag::NameId work_us = diag::register_name("server.tick_work_us");
    diag::NameId overruns = diag::register_name("server.overruns");
    diag::NameId entities = diag::register_name("server.entities");
    diag::NameId memory = diag::register_name("server.memory_live_bytes");
    diag::NameId thought = diag::register_name("ai.thought");
    diag::NameId starved = diag::register_name("ai.starved");
    diag::NameId committed = diag::register_name("commands.committed");
    diag::NameId refused = diag::register_name("commands.refused");
    diag::NameId bodies = diag::register_name("physics.bodies");
    diag::NameId active = diag::register_name("physics.active_bodies");
    diag::NameId state = diag::register_name("server.state_hash");
    // Where a tick's time went, so an overrun names its phase rather than only its size.
    diag::NameId ai_us = diag::register_name("server.ai_us");
    diag::NameId commands_us = diag::register_name("server.commands_us");
    diag::NameId physics_us = diag::register_name("server.physics_us");

    void count(diag::NameId name, u64 value) const noexcept {
        diag::trace_counter(name, category, diag::Channel::Important, value);
    }
};

[[nodiscard]] u64 live_bytes() noexcept {
    u64 total = 0;
    for (u32 domain = 0; domain < static_cast<u32>(MemoryDomain::Count); ++domain) {
        total += system_allocator(static_cast<MemoryDomain>(domain)).live_bytes();
    }
    return total;
}

struct Move {
    f32 vx = 0.0F;
    f32 vz = 0.0F;
};

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        return 2;
    }
    const u32 count = options.entities;

    diag::TraceConfig trace_config;
    trace_config.path = options.trace.c_str();
    trace_config.build_identity = "cy_headless_server";
    if (const Expected<diag::TraceId, Error> opened = diag::trace_open(trace_config); !opened) {
        return fail("the trace could not be opened", opened.error());
    }
    const Counters counters;

    // --- AI
    // ---------------------------------------------------------------------------------------
    graph::NodeRegistry registry(allocator());
    if (const Status registered = graph::behaviour::register_behaviour_nodes(registry);
        !registered) {
        return fail("the behaviour nodes could not be registered", registered.error());
    }
    graph::DiagnosticSink sink(allocator());
    Expected<graph::behaviour::BehaviourProgram, Error> program = behaviour(registry, sink);
    if (!program) {
        return fail("the behaviour did not compile", program.error());
    }

    // Tiered as `ai-system` intends a crowd to be: a few agents every tick, most of them rarely,
    // so 100 000 agents cost a bounded, budgeted number of thinks and none of them starves.
    ai::AiBudget budget;
    budget.thinks_per_tick[static_cast<usize>(ai::AiTier::Full)] = 2000;
    budget.thinks_per_tick[static_cast<usize>(ai::AiTier::Reduced)] = 2000;
    budget.thinks_per_tick[static_cast<usize>(ai::AiTier::Minimal)] = 1000;
    budget.thinks_per_tick[static_cast<usize>(ai::AiTier::Statistical)] = 1000;
    ai::AiRuntime runtime(allocator(), budget, ai::TierPolicy{});
    const Expected<u32, Error> first = runtime.reserve_slots(*program, count);
    if (!first) {
        return fail("the AI slots could not be reserved", first.error());
    }
    if (const Status sized = runtime.set_history_capacity(8192); !sized) {
        return fail("the decision history could not be sized", sized.error());
    }
    std::vector<Entity> entities(count);
    std::vector<ai::AIAgent> agents(count);
    std::vector<ai::AIState> states(count);
    std::vector<ai::Blackboard> blackboards(count);
    for (u32 index = 0; index < count; ++index) {
        entities[index] = Entity::make(index + 1U, 1);
        agents[index].graph = Name::intern("sentry");
        agents[index].tier = tier_for(index);
        states[index].slot = *first + index;
    }

    // --- Commands: one AI source, a group binding per squad of a thousand
    // -------------------------
    gameplay::GameSession session(allocator(), 0x5E7FULL);
    gameplay::ControlRegistry control(allocator());
    gameplay::CommandStream commands(allocator(), control);
    const auto host_agent =
        session.add_participant(gameplay::ParticipantKind::ServerAgent, Name::intern("server"));
    if (!host_agent) {
        return fail("the server participant could not be added", host_agent.error());
    }
    const auto brain = control.create_source(gameplay::ControlSourceKind::ArtificialIntelligence,
                                             *host_agent, Name::intern("ai"));
    const auto producer = commands.open_producer(Name::intern("ai"));
    gameplay::CommandDeclaration declaration;
    declaration.name = Name::intern("Reposition");
    declaration.stable_id = 91;
    declaration.channel = gameplay::channels::movement();
    const auto move = commands.declare(declaration);
    if (!brain || !producer || !move) {
        std::fprintf(stderr, "%s: the command stream could not be set up\n", kTag);
        return 1;
    }
    constexpr u32 kSquad = 1000;
    for (u32 base = 0; base < count; base += kSquad) {
        const auto squad = control.create_group(Name::intern("squad"));
        if (!squad) {
            return fail("a squad could not be created", squad.error());
        }
        for (u32 index = base; index < base + kSquad && index < count; ++index) {
            if (const Status added = control.add_to_group(*squad, entities[index]); !added) {
                return fail("a squad member could not be added", added.error());
            }
        }
        if (const Status bound = control.bind_group(*brain, gameplay::channels::movement(), *squad);
            !bound) {
            return fail("a squad could not be bound", bound.error());
        }
    }

    // --- Physics: every entity a body, resting on the ground
    // ---------------------------------------
    const Expected<physics::PhysicsServer*, Error> made =
        physics::jolt::create_server(allocator(), nullptr);
    if (!made) {
        return fail("the physics server could not be created", made.error());
    }
    physics::PhysicsServer& world_physics = **made;
    if (const Status started = world_physics.initialize(); !started) {
        return fail("the physics server did not initialise", started.error());
    }
    physics::WorldDescription world_description;
    world_description.name = Name::intern("dedicated-server");
    world_description.body_capacity = count + 16U;
    world_description.body_pair_capacity = count * 4U;
    world_description.contact_constraint_capacity = count * 2U;
    const auto world = world_physics.create_world(world_description);
    physics::ShapeDescription ball;
    ball.type = physics::ShapeType::Sphere;
    ball.radius = 0.4F;
    physics::ShapeDescription floor;
    floor.type = physics::ShapeType::Box;
    floor.half_extents = Vec3{2000.0F, 1.0F, 2000.0F};
    const auto ball_shape = world_physics.create_shape(ball);
    const auto floor_shape = world_physics.create_shape(floor);
    if (!world || !ball_shape || !floor_shape) {
        std::fprintf(stderr, "%s: the physics world could not be built\n", kTag);
        return 1;
    }
    physics::ColliderDescription floor_collider;
    floor_collider.shape = *floor_shape;
    physics::BodyDescription ground;
    ground.motion = physics::MotionType::Static;
    ground.transform.translation = Vec3{0.0F, -1.0F, 0.0F};
    ground.colliders = &floor_collider;
    ground.collider_count = 1;
    if (const auto placed = world_physics.create_body(*world, ground); !placed) {
        return fail("the ground could not be placed", placed.error());
    }
    // Units collide with the ground and not with each other: a strategy server steers its units
    // apart (avoidance is the AI's and the navigation's job), and rigid contacts between 100 000
    // tightly packed bodies made every order wake its neighbours in a cascade — measured, the step
    // went from 8 ms to 140 ms in thirty ticks as the woken set grew.
    physics::ColliderDescription ball_collider;
    ball_collider.shape = *ball_shape;
    ball_collider.filter = physics::CollisionFilter{1, 1U << 0U};
    std::vector<physics::BodyDescription> descriptions(count);
    for (u32 index = 0; index < count; ++index) {
        physics::BodyDescription& body = descriptions[index];
        // KINEMATIC: moved by the gameplay that ordered it and never by the solver, which is how a
        // strategy server drives units. Measured as dynamic bodies, the woken set's contacts took
        // the step past the tick budget within a second of orders.
        body.motion = physics::MotionType::Kinematic;
        // A 400-wide grid, two metres apart: column and row are whole numbers on purpose.
        const u32 column = index % 400U;
        const u32 row = index / 400U;
        body.transform.translation = Vec3{(static_cast<f32>(column) * 2.0F) - 400.0F, 0.4F,
                                          (static_cast<f32>(row) * 2.0F) - 250.0F};
        body.colliders = &ball_collider;
        body.collider_count = 1;
        body.start_asleep = true;
    }
    std::vector<physics::BodyHandle> bodies(count);
    // The tick each unit's current order runs out, zero when it has none.
    std::vector<u32> order_ends(count, 0U);
    if (const Status created = world_physics.create_bodies(
            *world, Span<const physics::BodyDescription>(descriptions.data(), count),
            Span<physics::BodyHandle>(bodies.data(), count));
        !created) {
        return fail("the bodies could not be created", created.error());
    }
    descriptions.clear();

    std::printf("%s: dedicated server — no renderer, GPU, audio or interface linked\n", kTag);
    std::printf("%s: entities %u, ai agents %u, bodies %u, squads %u, rate %u Hz, %s\n", kTag,
                count, count, count, (count + kSquad - 1) / kSquad, options.rate,
                options.paced ? "paced" : "unpaced");
    std::printf(
        "%s: NOT EXERCISED connections, replication, world streaming — no transport and "
        "one resident world\n",
        kTag);

    // --- The fixed-rate loop
    // ----------------------------------------------------------------------
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1'000'000'000LL / options.rate);
    const f32 dt = 1.0F / static_cast<f32>(options.rate);
    Host host;
    u64 overruns = 0;
    u64 committed_total = 0;
    u64 thought_total = 0;
    u64 starved_total = 0;
    u64 worst_work_us = 0;
    u64 state_hash = 0xcbf29ce484222325ULL;
    const Clock::time_point started = Clock::now();
    Clock::time_point next = started;

    for (u32 tick = 1; tick <= options.ticks; ++tick) {
        const Clock::time_point began = Clock::now();
        diag::trace_tick_begin(tick);

        ai::ThinkReport report;
        if (const Status thought = runtime.think(
                tick, *program, Span<const Entity>(entities.data(), count),
                Span<ai::AIAgent>(agents.data(), count), Span<ai::AIState>(states.data(), count),
                Span<ai::Blackboard>(blackboards.data(), count), Span<ai::KnowledgeStore*>(), host,
                dt, report);
            !thought) {
            return fail("the AI could not think", thought.error());
        }

        // Every agent that decided THIS tick becomes a command. The history is the AI's own record
        // of who decided what, so the command names the agent that made the decision.
        const Clock::time_point commands_began = Clock::now();
        gameplay::CommandBuffer& buffer = commands.producer(*producer);
        for (const ai::DecisionRecord& decision : runtime.history()) {
            if (decision.tick != tick || decision.status != graph::behaviour::BtStatus::Success) {
                continue;
            }
            gameplay::Command command;
            command.type = *move;
            command.tick = tick;
            command.participant = *host_agent;
            command.source = *brain;
            command.target = decision.agent;
            const u32 lane = decision.agent.index() % 8U;
            (void)command.set_payload(Move{static_cast<f32>(lane) - 3.5F, 1.0F});
            (void)buffer.record(command);
        }
        gameplay::GameplayContext context;
        context.session = &session;
        context.services = &session.services();
        context.commands = &commands;
        context.at.tick = tick;
        commands.commit(context, tick);
        for (u32 index = 0; index < commands.committed_count(); ++index) {
            const gameplay::Command& command = commands.committed(index);
            Move intent;
            if (!command.read_payload(intent)) {
                continue;
            }
            const u32 unit = command.target.index() - 1U;
            (void)world_physics.set_body_velocity(bodies[unit], Vec3{intent.vx, 0.0F, intent.vz},
                                                  Vec3{0.0F, 0.0F, 0.0F});
            order_ends[unit] = tick + kOrderTicks;
            state_hash = (state_hash ^ command.target.bits()) * 0x100000001b3ULL;
        }
        commands.log().clear();
        // Orders that ran out this tick: the unit stops where it is.
        for (u32 unit = 0; unit < count; ++unit) {
            if (order_ends[unit] == tick) {
                (void)world_physics.set_body_velocity(bodies[unit], Vec3{0.0F, 0.0F, 0.0F},
                                                      Vec3{0.0F, 0.0F, 0.0F});
                order_ends[unit] = 0;
            }
        }

        const Clock::time_point physics_began = Clock::now();
        physics::StepInput step;
        step.delta_seconds = dt;
        step.tick = tick;
        if (const Status stepped = world_physics.step(*world, step); !stepped) {
            return fail("the physics world could not step", stepped.error());
        }
        const auto statistics = world_physics.statistics(*world);

        const Clock::time_point ended = Clock::now();
        const auto us = [](Clock::time_point from, Clock::time_point to) {
            return static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
        };
        counters.count(counters.ai_us, us(began, commands_began));
        counters.count(counters.commands_us, us(commands_began, physics_began));
        counters.count(counters.physics_us, us(physics_began, ended));
        const u64 work_us = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - began).count());
        worst_work_us = work_us > worst_work_us ? work_us : worst_work_us;
        const bool overran = std::chrono::microseconds(work_us) > period;
        overruns += overran ? 1U : 0U;
        u32 thought = 0;
        for (const u32 tier : report.thought) {
            thought += tier;
        }
        thought_total += thought;
        starved_total += report.starved;
        committed_total += commands.committed_count();

        counters.count(counters.work_us, work_us);
        counters.count(counters.overruns, overruns);
        counters.count(counters.entities, count);
        counters.count(counters.memory, live_bytes());
        counters.count(counters.thought, thought);
        counters.count(counters.starved, report.starved);
        counters.count(counters.committed, commands.committed_count());
        counters.count(counters.refused, commands.rejection_count());
        counters.count(counters.bodies, statistics ? statistics->body_count : 0U);
        counters.count(counters.active, statistics ? statistics->active_body_count : 0U);
        diag::trace_state_hash(counters.state, state_hash);
        diag::trace_tick_end(tick);

        if (options.paced) {
            next += period;
            std::this_thread::sleep_until(next);
        }
        const f64 seconds = std::chrono::duration<f64>(Clock::now() - started).count();
        counters.count(counters.tick_rate,
                       seconds > 0.0 ? static_cast<u64>(static_cast<f64>(tick) / seconds) : 0U);
    }

    const f64 seconds = std::chrono::duration<f64>(Clock::now() - started).count();
    const Expected<diag::TraceStats, Error> closed = diag::trace_close();
    world_physics.shutdown();
    physics::jolt::destroy_server(&world_physics, allocator());
    if (!closed) {
        return fail("the trace could not be closed", closed.error());
    }

    std::printf(
        "%s: ticks %u in %.2f s — %.1f Hz achieved, %u Hz fixed, %llu overrun(s), worst "
        "tick %llu us\n",
        kTag, options.ticks, seconds, static_cast<f64>(options.ticks) / seconds, options.rate,
        static_cast<unsigned long long>(overruns), static_cast<unsigned long long>(worst_work_us));
    std::printf("%s: ai thought %llu, starved %llu; commands committed %llu; state %016llx\n", kTag,
                static_cast<unsigned long long>(thought_total),
                static_cast<unsigned long long>(starved_total),
                static_cast<unsigned long long>(committed_total),
                static_cast<unsigned long long>(state_hash));
    std::printf("%s: trace %s — %llu events, %llu bytes\n", kTag, options.trace.c_str(),
                static_cast<unsigned long long>(closed->events_written),
                static_cast<unsigned long long>(closed->bytes_written));
    return starved_total == 0 && committed_total > 0 ? 0 : 1;
}
