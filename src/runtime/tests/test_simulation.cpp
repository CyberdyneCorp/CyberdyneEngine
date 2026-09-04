// The tick pipeline, the commit boundary, the deferred frame queue, and the state hash over a real
// world. Tasks 4.1.1, 4.1.2, 4.1.3, 4.2.3 and 4.2.6.
//
// Integration and not unit: every case here brings up a `World` and most bring up a `SceneTree`,
// and a world's first archetype is a 16 KiB chunk allocation. Measured at 0.4-2.6 ms per case in
// Debug against a unit budget of 1 ms.

#include <cy/core/memory/system_allocator.h>
#include <cy/runtime/simulation.h>
#include <cy/test/test.h>

#include "fixtures.h"

#include <cstdint>
#include <memory>
#include <string_view>

namespace {

using namespace cy;
using namespace cy::runtime;
using cy::runtime::test::flash_type;
using cy::runtime::test::Position;
using cy::runtime::test::position_type;

/// What the systems record, so that "the four fixed stages ran in this order" is an assertion.
struct Journal {
    ecs::Stage stages[16] = {};
    u32 count = 0;
    u64 ticks_seen[16] = {};
    const Simulation* simulation = nullptr;
};

void record(const ecs::SystemContext& context) noexcept {
    auto* journal = static_cast<Journal*>(context.user);
    if (journal->count >= 16) {
        return;
    }
    journal->stages[journal->count] = context.stage;
    // A system reads simulation time from the clock and never a wall clock. There is nothing else
    // it *could* read: `Simulation::clock()` is const and `SimulationClock` has no member that
    // calls a clock.
    journal->ticks_seen[journal->count] = journal->simulation->clock().tick();
    ++journal->count;
}

Status add_system(Simulation& simulation, ecs::Stage stage, const char* name,
                  Journal& journal) noexcept {
    ecs::SystemDesc desc;
    desc.name = name;
    desc.body = &record;
    desc.user = &journal;
    const auto added = simulation.schedule().add(stage, desc);
    return added ? ok() : Unexpected<Error>(added.error());
}

SimulationConfig fixed_step_config(u64 seed = 1, bool tree = true) {
    SimulationConfig config;
    config.world_name = "test-world";
    config.session_seed = seed;
    config.create_scene_tree = tree;
    config.clock.mode = determinism::TickMode::FixedStep;
    config.clock.fixed_ticks_per_frame = 1;
    return config;
}

/// A simulation with the two fixture components registered, brought up and closed.
///
/// `registration_order` exists for one case and is worth the parameter: a `ComponentTypeId` is the
/// index the registry handed out, so registering the same two types the other way round gives them
/// each other's numbers without changing anything a game can observe. See the regression case
/// "the hash does not depend on the order components were registered".
enum class Registration : cy::u8 { PositionFirst, FlashFirst };

struct Fixture {
    explicit Fixture(const SimulationConfig& config,
                     Registration order = Registration::PositionFirst)
        : simulation(system_allocator(MemoryDomain::Ecs), config) {
        CY_REQUIRE(static_cast<bool>(simulation.initialize()));
        // `register_reflected` and not `register_component<T>()`: the latter resolves the
        // descriptor through `reflect::type_of<T>()`, which needs the generator, and these
        // fixtures are hand-written for the reason fixtures.h gives.
        ecs::ComponentRegistry& registry = simulation.world().components();
        if (order == Registration::FlashFirst) {
            const auto flash = registry.register_reflected(flash_type());
            CY_REQUIRE(static_cast<bool>(flash));
            flash_component = flash.value();
        }
        const auto registered = registry.register_reflected(position_type());
        CY_REQUIRE(static_cast<bool>(registered));
        position = registered.value();
        if (order == Registration::PositionFirst) {
            const auto flash = registry.register_reflected(flash_type());
            CY_REQUIRE(static_cast<bool>(flash));
            flash_component = flash.value();
        }
    }

    void close() { CY_REQUIRE(static_cast<bool>(simulation.finalize_registration())); }

    [[nodiscard]] ecs::Entity spawn(f32 x, f32 y) {
        const ecs::ComponentTypeId components[] = {position};
        const auto entity =
            simulation.world().create(Span<const ecs::ComponentTypeId>(components, 1));
        CY_REQUIRE(static_cast<bool>(entity));
        const Position value{x, y, true, 0};
        CY_REQUIRE(static_cast<bool>(simulation.world().set(entity.value(), position, value)));
        return entity.value();
    }

    Simulation simulation;
    ecs::ComponentTypeId position = 0;
    ecs::ComponentTypeId flash_component = 0;
};

}  // namespace

CY_TEST_CASE("a tick runs the four fixed stages and commits exactly once") {
    Fixture fixture(fixed_step_config(1, /*tree=*/false));
    Journal journal;
    journal.simulation = &fixture.simulation;
    CY_REQUIRE(static_cast<bool>(
        add_system(fixture.simulation, ecs::Stage::PostSimulation, "post", journal)));
    CY_REQUIRE(
        static_cast<bool>(add_system(fixture.simulation, ecs::Stage::Physics, "phys", journal)));
    CY_REQUIRE(static_cast<bool>(
        add_system(fixture.simulation, ecs::Stage::PreSimulation, "pre", journal)));
    CY_REQUIRE(
        static_cast<bool>(add_system(fixture.simulation, ecs::Stage::Simulation, "sim", journal)));
    fixture.close();

    const auto committed = fixture.simulation.step(nullptr);
    CY_REQUIRE(static_cast<bool>(committed));

    // Registration order within the schedule does not decide stage order: the pipeline does.
    CY_REQUIRE_EQ(journal.count, 4U);
    CY_CHECK(journal.stages[0] == ecs::Stage::PreSimulation);
    CY_CHECK(journal.stages[1] == ecs::Stage::Physics);
    CY_CHECK(journal.stages[2] == ecs::Stage::Simulation);
    CY_CHECK(journal.stages[3] == ecs::Stage::PostSimulation);

    // One commit per tick, and the state version is the tick's, not the world's per-stage version.
    CY_CHECK_EQ(committed.value().state_version, 1ULL);
    CY_CHECK_EQ(committed.value().point.tick, 1ULL);
    CY_CHECK_EQ(fixture.simulation.committed().state_version, 1ULL);
}

CY_TEST_CASE("the variable half runs the other four stages") {
    Fixture fixture(fixed_step_config(1, /*tree=*/false));
    Journal journal;
    journal.simulation = &fixture.simulation;
    for (const auto stage :
         {ecs::Stage::Render, ecs::Stage::UI, ecs::Stage::Animation, ecs::Stage::Frame}) {
        CY_REQUIRE(static_cast<bool>(
            add_system(fixture.simulation, stage, ecs::stage_name(stage), journal)));
    }
    fixture.close();

    CY_REQUIRE(static_cast<bool>(fixture.simulation.frame(0.5F, nullptr)));
    CY_REQUIRE_EQ(journal.count, 4U);
    CY_CHECK(journal.stages[0] == ecs::Stage::Frame);
    CY_CHECK(journal.stages[1] == ecs::Stage::Animation);
    CY_CHECK(journal.stages[2] == ecs::Stage::UI);
    CY_CHECK(journal.stages[3] == ecs::Stage::Render);

    // The variable half does not commit: `simulation-and-determinism` puts the boundary in the
    // tick.
    CY_CHECK_EQ(fixture.simulation.committed().state_version, 0ULL);
}

CY_TEST_CASE("every system reads simulation time from the clock") {
    Fixture fixture(fixed_step_config(1, /*tree=*/false));
    Journal journal;
    journal.simulation = &fixture.simulation;
    CY_REQUIRE(
        static_cast<bool>(add_system(fixture.simulation, ecs::Stage::Simulation, "sim", journal)));
    fixture.close();

    for (u32 tick = 0; tick < 3; ++tick) {
        CY_REQUIRE(static_cast<bool>(fixture.simulation.step(nullptr)));
    }
    CY_REQUIRE_EQ(journal.count, 3U);
    // The tick a system sees is the tick being simulated — the second of three catch-up ticks is
    // told it is the second, not the third.
    CY_CHECK_EQ(journal.ticks_seen[0], 1ULL);
    CY_CHECK_EQ(journal.ticks_seen[1], 2ULL);
    CY_CHECK_EQ(journal.ticks_seen[2], 3ULL);
}

CY_TEST_CASE("a tick before registration is closed is refused") {
    Fixture fixture(fixed_step_config(1, /*tree=*/false));
    // Not closed: a registry that closed itself when the first tick arrived would close at a point
    // that depends on when the first tick happened to arrive.
    CY_CHECK_FALSE(static_cast<bool>(fixture.simulation.step(nullptr)));
    CY_CHECK_FALSE(static_cast<bool>(fixture.simulation.frame(0.0F, nullptr)));
    CY_CHECK_FALSE(fixture.simulation.registration_closed());

    fixture.close();
    CY_CHECK(fixture.simulation.registration_closed());
    CY_CHECK(static_cast<bool>(fixture.simulation.step(nullptr)));
}

CY_TEST_CASE("commit observers are told once, about the same tick") {
    // The five consumers `simulation-and-determinism` names all key off this one point rather than
    // defining their own moment.
    class Consumer final : public determinism::CommitObserver {
    public:
        explicit Consumer(const char* name) noexcept : name_(name) {}
        [[nodiscard]] const char* name() const noexcept override { return name_; }
        [[nodiscard]] Status on_commit(const determinism::CommitRecord& record) noexcept override {
            ++calls;
            last = record;
            return ok();
        }
        u32 calls = 0;
        determinism::CommitRecord last;

    private:
        const char* name_;
    };

    Fixture fixture(fixed_step_config(1, /*tree=*/false));
    Consumer hasher("hash");
    Consumer saver("save");
    CY_REQUIRE(static_cast<bool>(fixture.simulation.commit_boundary().observe(hasher)));
    CY_REQUIRE(static_cast<bool>(fixture.simulation.commit_boundary().observe(saver)));
    fixture.close();

    CY_REQUIRE(static_cast<bool>(fixture.simulation.step(nullptr)));
    CY_REQUIRE(static_cast<bool>(fixture.simulation.step(nullptr)));

    CY_CHECK_EQ(hasher.calls, 2U);
    CY_CHECK_EQ(saver.calls, 2U);
    CY_CHECK(hasher.last.point == saver.last.point);
    CY_CHECK_EQ(hasher.last.state_version, saver.last.state_version);
    CY_CHECK_EQ(hasher.last.point.tick, 2ULL);
}

CY_TEST_CASE("frame commands are deferred to the flush point, in submission order") {
    // `engine-architecture`: node reparenting and scene loading are applied "at defined flush
    // points, in submission order".
    struct Recorder {
        u32 order[8] = {};
        u32 count = 0;
    };
    static Recorder recorder;
    recorder = Recorder{};

    Fixture fixture(fixed_step_config(1, /*tree=*/true));
    fixture.close();
    CY_REQUIRE(fixture.simulation.tree() != nullptr);

    // The tag is the address of an element of a local array rather than an integer cast to a
    // pointer: `user` is a `void*`, and laundering a small integer through one is a pessimisation
    // the linter is right about.
    u32 tags[4] = {1, 2, 3, 4};
    const auto append = [](scene::SceneTree& /*tree*/, void* user) noexcept -> Status {
        if (recorder.count < 8) {
            recorder.order[recorder.count++] = *static_cast<const u32*>(user);
        }
        return ok();
    };

    FrameCommandQueue& commands = fixture.simulation.commands();
    for (u32& tag : tags) {
        CY_REQUIRE(static_cast<bool>(commands.call(append, &tag)));
    }
    // Nothing has run: the queue is deferred, not immediate.
    CY_CHECK_EQ(recorder.count, 0U);
    CY_CHECK_EQ(commands.pending(), 4U);

    CY_REQUIRE(static_cast<bool>(fixture.simulation.step(nullptr)));
    CY_CHECK_EQ(commands.pending(), 0U);
    CY_REQUIRE_EQ(recorder.count, 4U);
    for (u32 index = 0; index < 4; ++index) {
        CY_CHECK_EQ(recorder.order[index], index + 1);
    }
    CY_CHECK_EQ(fixture.simulation.last_tick().frame_commands_applied, 4U);
}

CY_TEST_CASE("a command recorded during a flush waits for the next one") {
    // The one failure mode a deferred queue must not have is an unbounded flush.
    struct Reentrant {
        FrameCommandQueue* queue = nullptr;
        u32 runs = 0;
    };
    static Reentrant reentrant;
    reentrant = Reentrant{};

    Fixture fixture(fixed_step_config(1, /*tree=*/true));
    fixture.close();
    reentrant.queue = &fixture.simulation.commands();

    static FrameCommandFn again = nullptr;
    again = [](scene::SceneTree& /*tree*/, void* /*user*/) noexcept -> Status {
        ++reentrant.runs;
        if (reentrant.runs < 3) {
            return reentrant.queue->call(again, nullptr);
        }
        return ok();
    };

    CY_REQUIRE(static_cast<bool>(reentrant.queue->call(again, nullptr)));
    CY_REQUIRE(static_cast<bool>(fixture.simulation.step(nullptr)));
    CY_CHECK_EQ(reentrant.runs, 1U);
    CY_CHECK_EQ(reentrant.queue->pending(), 1U);

    CY_REQUIRE(static_cast<bool>(fixture.simulation.step(nullptr)));
    CY_CHECK_EQ(reentrant.runs, 2U);

    // The static holds a pointer into this case's stack frame; cleared here so that it does not
    // outlive the fixture. A file-scope object is the only way to give a plain function pointer
    // state, and this is what that costs.
    reentrant.queue = nullptr;
}

CY_TEST_CASE("a failing frame command does not stop the ones behind it") {
    struct Counter {
        u32 runs = 0;
    };
    static Counter counter;
    counter = Counter{};

    Fixture fixture(fixed_step_config(1, /*tree=*/true));
    fixture.close();

    const auto succeed = [](scene::SceneTree&, void*) noexcept -> Status {
        ++counter.runs;
        return ok();
    };
    const auto blow_up = [](scene::SceneTree&, void*) noexcept -> Status {
        return fail(ErrorCode::Internal, "no");
    };

    FrameCommandQueue& commands = fixture.simulation.commands();
    CY_REQUIRE(static_cast<bool>(commands.call(blow_up, nullptr)));
    CY_REQUIRE(static_cast<bool>(commands.call(succeed, nullptr)));
    CY_REQUIRE(static_cast<bool>(commands.call(succeed, nullptr)));
    CY_REQUIRE(static_cast<bool>(fixture.simulation.step(nullptr)));

    CY_CHECK_EQ(counter.runs, 2U);
    CY_CHECK_EQ(fixture.simulation.last_tick().frame_commands_failed, 1U);
    CY_CHECK_EQ(commands.pending(), 0U);
}

CY_TEST_CASE("the state hash covers declared authoritative fields and nothing else") {
    Fixture fixture(fixed_step_config(1, /*tree=*/false));
    const ecs::Entity first = fixture.spawn(1.0F, 2.0F);
    fixture.close();

    const auto initial = fixture.simulation.hash_now();
    CY_REQUIRE(static_cast<bool>(initial));

    // `revision` is Derived — recomputed, never hashed — so writing it changes nothing.
    auto* value = fixture.simulation.world().get_mut<Position>(first, fixture.position);
    CY_REQUIRE(value != nullptr);
    value->revision = 99;
    const auto after_derived = fixture.simulation.hash_now();
    CY_REQUIRE(static_cast<bool>(after_derived));
    CY_CHECK_EQ(after_derived.value(), initial.value());

    // `x` is RuntimeState, which the simulation classifies authoritative. Writing it does change
    // it.
    value->x = 1.5F;
    const auto after_authoritative = fixture.simulation.hash_now();
    CY_REQUIRE(static_cast<bool>(after_authoritative));
    CY_CHECK_NE(after_authoritative.value(), initial.value());

    const WorldHashReport& report = fixture.simulation.last_hash_report();
    CY_CHECK_EQ(report.entities_hashed, 1U);
    CY_CHECK_EQ(report.components_hashed, 1U);
    // x, y and alive: three of Position's four fields. `revision` is Derived and is not one of
    // them.
    CY_CHECK_EQ(report.fields_hashed, 3U);
    // The ECS's Parent and Children are built-ins with no reflected descriptor, so the hash is
    // silent about them and says so rather than pretending otherwise.
    CY_CHECK_GT(report.subjects_undeclared, 0U);
}

CY_TEST_CASE("two independent worlds built the same way hash the same") {
    // Milestone gate 6.1's first half, inside one process. The across-processes half is
    // `smoke.tick_loop`.
    const auto build = [](u64 seed, f32 x) {
        auto fixture = std::make_unique<Fixture>(fixed_step_config(seed, /*tree=*/false));
        (void)fixture->spawn(x, 2.0F);
        (void)fixture->spawn(3.0F, 4.0F);
        fixture->close();
        return fixture;
    };

    const auto left = build(7, 1.0F);
    const auto right = build(7, 1.0F);
    const auto left_hash = left->simulation.hash_now();
    const auto right_hash = right->simulation.hash_now();
    CY_REQUIRE(static_cast<bool>(left_hash));
    CY_REQUIRE(static_cast<bool>(right_hash));
    CY_CHECK_EQ(left_hash.value(), right_hash.value());

    // A different session seed diverges at the root, in a named subsystem, rather than somewhere in
    // the entity data — that is what registering the random source as a state provider buys.
    const auto other_seed = build(8, 1.0F);
    const auto other_hash = other_seed->simulation.hash_now();
    CY_REQUIRE(static_cast<bool>(other_hash));
    CY_CHECK_NE(other_hash.value(), left_hash.value());

    determinism::Divergence divergence;
    determinism::StateHashTree::compare(left->simulation.last_hash_tree(),
                                        other_seed->simulation.last_hash_tree(), divergence);
    CY_REQUIRE(divergence.diverged);
    CY_CHECK_GE(divergence.depth, 2U);
    CY_REQUIRE(divergence.depth >= 2U);
    CY_CHECK(divergence.levels[1] == determinism::HashLevel::Subsystem);
    CY_CHECK(std::string_view(divergence.names[1]) == "simulation.random");
}

CY_TEST_CASE("the hash does not depend on the order components were registered") {
    // REGRESSION, found at M2's close by an adversarial probe rather than by a test.
    //
    // `simulation-and-determinism` — "Registration and initialisation order": registries whose
    // contents affect simulation, types among them, "SHALL be finalised in a deterministic order
    // derived from stable identifiers", and "WHEN plugins load in a different order THEN simulation
    // results SHALL be unchanged". The state hash folded `ComponentTypeId`s, which are the indices
    // `ComponentRegistry` hands out in registration order — so these two worlds, whose content is
    // identical in every respect a game can observe, hashed differently. Build-time feature slicing
    // is the shipping case: dropping one module's components shifts every later id, and a sliced
    // build could not compare hashes with a full one.
    //
    // The fix is in src/runtime/src/state_hash.cpp: fold the component's `reflect::TypeId` (or, for
    // a built-in, an unseeded hash of its registered name), sort the identities before folding
    // them, and walk an entity's components in that order rather than in column order.
    const auto build = [](Registration order) {
        auto fixture = std::make_unique<Fixture>(fixed_step_config(7, /*tree=*/false), order);
        (void)fixture->spawn(1.0F, 2.0F);
        (void)fixture->spawn(3.0F, 4.0F);
        fixture->close();
        return fixture;
    };

    const auto forward = build(Registration::PositionFirst);
    const auto reversed = build(Registration::FlashFirst);
    // The premise: the two worlds really did give the same type different numbers.
    CY_CHECK_NE(forward->position, reversed->position);
    CY_CHECK_NE(forward->flash_component, reversed->flash_component);

    const auto forward_hash = forward->simulation.hash_now();
    const auto reversed_hash = reversed->simulation.hash_now();
    CY_REQUIRE(static_cast<bool>(forward_hash));
    CY_REQUIRE(static_cast<bool>(reversed_hash));
    CY_CHECK_EQ(forward_hash.value(), reversed_hash.value());
}

CY_TEST_CASE("a divergence narrows to the entity and the field that differ") {
    const auto build = [](f32 y) {
        auto fixture = std::make_unique<Fixture>(fixed_step_config(7, /*tree=*/false));
        (void)fixture->spawn(1.0F, 2.0F);
        (void)fixture->spawn(3.0F, y);
        fixture->close();
        (void)fixture->simulation.hash_now();
        return fixture;
    };

    const auto left = build(4.0F);
    const auto right = build(4.5F);

    determinism::Divergence divergence;
    determinism::StateHashTree::compare(left->simulation.last_hash_tree(),
                                        right->simulation.last_hash_tree(), divergence);
    CY_REQUIRE(divergence.diverged);
    CY_CHECK_FALSE(divergence.shape_mismatch);

    // world -> archetype -> entity -> component -> field.
    CY_REQUIRE_EQ(divergence.depth, 5U);
    CY_CHECK(divergence.levels[2] == determinism::HashLevel::Entity);
    CY_CHECK(divergence.levels[4] == determinism::HashLevel::Field);
    CY_CHECK(std::string_view(divergence.names[3]) == "cy::runtime::test::Position");
    CY_CHECK(std::string_view(divergence.names[4]) == "y");
}

CY_TEST_CASE("the epoch moves and the same tick becomes a different moment") {
    Fixture fixture(fixed_step_config(1, /*tree=*/false));
    fixture.close();
    CY_REQUIRE(static_cast<bool>(fixture.simulation.step(nullptr)));
    const determinism::SimulationPoint before = fixture.simulation.clock().now();

    const determinism::Epoch epoch =
        fixture.simulation.reset_epoch(determinism::EpochReason::CheckpointRestore, before.tick);
    CY_CHECK_EQ(epoch.value, before.epoch.value + 1);
    CY_CHECK_NE(fixture.simulation.clock().now(), before);
    CY_CHECK_EQ(fixture.simulation.clock().now().tick, before.tick);

    // And the hash changes with it, because the clock is a hashed state provider: two runs at the
    // same tick in different epochs are different states.
    const auto after = fixture.simulation.hash_now();
    CY_REQUIRE(static_cast<bool>(after));
    CY_CHECK_EQ(fixture.simulation.last_hash_report().archetypes_visited, 0U);
}

CY_TEST_CASE("a slow frame is bounded and a fixed-step frame ignores the clock") {
    SimulationConfig realtime;
    realtime.create_scene_tree = false;
    realtime.clock.max_ticks_per_frame = 8;
    Fixture bounded(realtime);
    bounded.close();

    const determinism::FrameTicks ticks = bounded.simulation.begin_frame(400'000'000);
    CY_CHECK_EQ(ticks.ticks, 8U);
    CY_CHECK_GT(ticks.discarded_ns, 0ULL);

    Fixture stepped(fixed_step_config(1, /*tree=*/false));
    stepped.close();
    CY_CHECK_EQ(stepped.simulation.begin_frame(0).ticks, 1U);
    CY_CHECK_EQ(stepped.simulation.begin_frame(9'999'999'999).ticks, 1U);
}

CY_TEST_CASE("a frame command with no scene tree is refused rather than accumulating") {
    Fixture fixture(fixed_step_config(1, /*tree=*/false));
    fixture.close();
    CY_CHECK(fixture.simulation.tree() == nullptr);

    const auto nothing = [](scene::SceneTree&, void*) noexcept -> Status { return ok(); };
    CY_REQUIRE(static_cast<bool>(fixture.simulation.commands().call(nothing, nullptr)));
    // Silently never applying it would leave the queue growing for the life of the process.
    CY_CHECK_FALSE(static_cast<bool>(fixture.simulation.step(nullptr)));
}
