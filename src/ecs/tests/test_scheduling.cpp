// The first real consumers of M1's conflict checker, running on the real job system. Task 2.5.
//
// Integration rather than unit: it starts worker threads.
//
// THE POINT OF THIS SUITE is that the parallel plan and the serial one produce the same world.
// Parallelism here is *derived* from the access declarations rather than declared by anyone, so if
// running the same schedule two ways gave two answers, the derivation would be wrong — and that is
// a claim only a test that runs it both ways can make.

#include <cy/test/test.h>

#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/query.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>

#include "fixtures.h"

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Ecs);
}

struct IntegrateState {
    cy::ecs::Query* query = nullptr;
    cy::ecs::ComponentTypeId position = cy::ecs::kInvalidComponent;
    cy::ecs::ComponentTypeId velocity = cy::ecs::kInvalidComponent;
};

void integrate(const cy::ecs::SystemContext& context) noexcept {
    auto* state = static_cast<IntegrateState*>(context.user);
    const cy::ecs::ComponentTypeId position = state->position;
    const cy::ecs::ComponentTypeId velocity = state->velocity;
    (void)state->query->for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
        const cy::Span<cy::ecs::test::Position> positions =
            chunk.write<cy::ecs::test::Position>(position);
        const cy::Span<const cy::ecs::test::Velocity> velocities =
            chunk.read<cy::ecs::test::Velocity>(velocity);
        for (cy::u32 row = 0; row < chunk.count(); ++row) {
            positions[row].x += velocities[row].x;
            positions[row].y += velocities[row].y;
        }
    });
}

struct CountState {
    cy::ecs::Query* query = nullptr;
    cy::u64 seen = 0;
};

void count_frozen(const cy::ecs::SystemContext& context) noexcept {
    auto* state = static_cast<CountState*>(context.user);
    state->seen = 0;
    (void)state->query->for_each_chunk(
        [&](cy::ecs::QueryChunk& chunk) { state->seen += chunk.count(); });
}

/// Build a world, its two systems and their schedule. Returned by value so the two halves of the
/// comparison below are set up by one piece of code and cannot drift apart.
struct Harness {
    cy::ecs::World world;
    cy::ecs::test::Components ids;

    explicit Harness(cy::Allocator& memory) noexcept : world(memory) {}
};

}  // namespace

CY_TEST_CASE("the parallel plan and the serial one produce the same world") {
    constexpr cy::u32 kEntities = 4096;
    constexpr cy::u32 kTicks = 8;

    cy::f32 parallel_x = 0.0F;
    cy::f32 serial_x = 0.0F;
    cy::u64 parallel_frozen = 0;
    cy::u64 serial_frozen = 0;

    for (int pass = 0; pass < 2; ++pass) {
        Harness harness(allocator());
        CY_REQUIRE(harness.world.initialize().has_value());
        const auto ids = cy::ecs::test::register_all(harness.world);
        CY_REQUIRE(ids.has_value());
        harness.ids = *ids;

        const cy::ecs::ComponentTypeId moving[] = {ids->position, ids->velocity};
        const cy::ecs::ComponentTypeId frozen[] = {ids->position, ids->velocity, ids->frozen};
        cy::Array<cy::ecs::Entity> created(allocator());
        CY_REQUIRE(harness.world
                       .create_many(kEntities, cy::Span<const cy::ecs::ComponentTypeId>(moving, 2),
                                    created)
                       .has_value());
        CY_REQUIRE(
            harness.world
                .create_many(128, cy::Span<const cy::ecs::ComponentTypeId>(frozen, 3), created)
                .has_value());
        for (cy::u32 index = 0; index < kEntities; ++index) {
            CY_REQUIRE(
                harness.world
                    .set(created[index], ids->velocity, cy::ecs::test::Velocity{1.0F, 2.0F, 0.0F})
                    .has_value());
        }

        cy::ecs::QueryDesc moving_desc(allocator());
        CY_REQUIRE(moving_desc.write(ids->position).has_value());
        CY_REQUIRE(moving_desc.read(ids->velocity).has_value());
        cy::ecs::Query moving_query(harness.world, std::move(moving_desc));

        cy::ecs::QueryDesc frozen_desc(allocator());
        CY_REQUIRE(frozen_desc.read(ids->velocity).has_value());
        CY_REQUIRE(frozen_desc.with(ids->frozen).has_value());
        cy::ecs::Query frozen_query(harness.world, std::move(frozen_desc));

        IntegrateState integrate_state;
        integrate_state.query = &moving_query;
        integrate_state.position = ids->position;
        integrate_state.velocity = ids->velocity;

        CountState count_state;
        count_state.query = &frozen_query;

        cy::ecs::Schedule schedule(harness.world);
        cy::ecs::SystemDesc first;
        first.name = "integrate";
        first.body = &integrate;
        first.user = &integrate_state;
        first.access = moving_query.desc().access();

        cy::ecs::SystemDesc second;
        second.name = "count-frozen";
        second.body = &count_frozen;
        second.user = &count_state;
        second.access = frozen_query.desc().access();

        const auto integrate_id = schedule.add(cy::ecs::Stage::Simulation, first);
        const auto count_id = schedule.add(cy::ecs::Stage::Simulation, second);
        CY_REQUIRE(integrate_id.has_value());
        CY_REQUIRE(count_id.has_value());
        CY_REQUIRE(schedule.build().has_value());

        // The two systems write and read different components, so the derivation puts them in one
        // batch and they really do run at the same time in the parallel pass.
        CY_CHECK_EQ(schedule.batch_count(cy::ecs::Stage::Simulation), 1u);

        if (pass == 0) {
            cy::jobs::JobSystemConfig config;
            config.worker_count = 4;
            cy::jobs::JobSystem jobs;
            CY_REQUIRE(jobs.start(config).has_value());
            for (cy::u32 tick = 0; tick < kTicks; ++tick) {
                CY_REQUIRE(schedule.run(cy::ecs::Stage::Simulation, jobs).has_value());
            }
            jobs.shutdown();
            parallel_x = cy::ecs::test::value_of<cy::ecs::test::Position>(harness.world, created[0],
                                                                          ids->position)
                             .x;
            parallel_frozen = count_state.seen;
        } else {
            for (cy::u32 tick = 0; tick < kTicks; ++tick) {
                CY_REQUIRE(schedule.run_serial(cy::ecs::Stage::Simulation).has_value());
            }
            serial_x = cy::ecs::test::value_of<cy::ecs::test::Position>(harness.world, created[0],
                                                                        ids->position)
                           .x;
            serial_frozen = count_state.seen;
        }
    }

    CY_CHECK_EQ(parallel_x, static_cast<cy::f32>(kTicks));
    CY_CHECK_EQ(parallel_x, serial_x);
    CY_CHECK_EQ(parallel_frozen, 128u);
    CY_CHECK_EQ(parallel_frozen, serial_frozen);
}

CY_TEST_CASE("systems spawning through their own command buffers merge in system-name order") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    struct SpawnState {
        cy::ecs::ComponentTypeId component = cy::ecs::kInvalidComponent;
        cy::f32 mark = 0.0F;
    };
    static const auto spawn = [](const cy::ecs::SystemContext& context) noexcept {
        auto* state = static_cast<SpawnState*>(context.user);
        auto created = context.commands->create();
        if (created) {
            (void)context.commands->add(*created, state->component,
                                        cy::ecs::test::Position{state->mark, 0.0F, 0.0F});
        }
    };

    SpawnState first_state{ids->position, 1.0F};
    SpawnState second_state{ids->position, 2.0F};

    cy::ecs::Schedule schedule(world);
    cy::ecs::SystemDesc first;
    first.name = "spawn-first";
    first.body = spawn;
    first.user = &first_state;
    CY_REQUIRE(first.access.write(ids->position).has_value());

    cy::ecs::SystemDesc second;
    second.name = "spawn-second";
    second.body = spawn;
    second.user = &second_state;
    CY_REQUIRE(second.access.write(ids->position).has_value());

    CY_REQUIRE(schedule.add(cy::ecs::Stage::Simulation, first).has_value());
    CY_REQUIRE(schedule.add(cy::ecs::Stage::Simulation, second).has_value());
    CY_REQUIRE(schedule.build().has_value());

    cy::jobs::JobSystemConfig config;
    config.worker_count = 4;
    cy::jobs::JobSystem jobs;
    CY_REQUIRE(jobs.start(config).has_value());
    CY_REQUIRE(schedule.run(cy::ecs::Stage::Simulation, jobs).has_value());
    jobs.shutdown();

    CY_CHECK_EQ(world.entity_count(), 2u);
    // The merge key is the systems' name order, not the order the workers happened to finish in:
    // "spawn-first" precedes "spawn-second" in the alphabet as well as in registration, so its
    // entity is created first, on every machine and every run. The case below is the one that
    // distinguishes the two rules.
    const cy::ecs::Entity first_created = world.entities().at(0);
    const cy::ecs::Entity second_created = world.entities().at(1);
    CY_CHECK_EQ(
        cy::ecs::test::value_of<cy::ecs::test::Position>(world, first_created, ids->position).x,
        1.0F);
    CY_CHECK_EQ(
        cy::ecs::test::value_of<cy::ecs::test::Position>(world, second_created, ids->position).x,
        2.0F);
}

CY_TEST_CASE("the flush order is the systems' names, not the order they were registered in") {
    // REGRESSION, task 1.7. The merge key used to be the system's registration index within its
    // stage. That is a sequence number standing in for an identity: it is reproducible only while
    // the same code registers the same systems in the same order, and the first plugin, feature
    // flag or game mode that registers one conditionally shifts every later key — two conflicting
    // spawns swap places in the flush and the world after the stage is a different world.
    //
    // So this case registers them in the order that disagrees with their names. Under the old rule
    // "zulu" flushed first because it was registered first; under the rule
    // `StateProviderRegistry::finalize()` established and `Schedule::build()` now follows, "alpha"
    // flushes first because it is called "alpha".
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    struct SpawnState {
        cy::ecs::ComponentTypeId component = cy::ecs::kInvalidComponent;
        cy::f32 mark = 0.0F;
    };
    static const auto spawn = [](const cy::ecs::SystemContext& context) noexcept {
        auto* state = static_cast<SpawnState*>(context.user);
        auto created = context.commands->create();
        if (created) {
            (void)context.commands->add(*created, state->component,
                                        cy::ecs::test::Position{state->mark, 0.0F, 0.0F});
        }
    };

    SpawnState zulu_state{ids->position, 1.0F};
    SpawnState alpha_state{ids->position, 2.0F};

    cy::ecs::Schedule schedule(world);
    cy::ecs::SystemDesc zulu;
    zulu.name = "spawn-zulu";
    zulu.body = spawn;
    zulu.user = &zulu_state;
    CY_REQUIRE(zulu.access.write(ids->position).has_value());

    cy::ecs::SystemDesc alpha;
    alpha.name = "spawn-alpha";
    alpha.body = spawn;
    alpha.user = &alpha_state;
    CY_REQUIRE(alpha.access.write(ids->position).has_value());

    // Registered zulu first, alpha second — the opposite of their name order.
    CY_REQUIRE(schedule.add(cy::ecs::Stage::Simulation, zulu).has_value());
    CY_REQUIRE(schedule.add(cy::ecs::Stage::Simulation, alpha).has_value());
    CY_REQUIRE(schedule.build().has_value());
    CY_REQUIRE(schedule.run_serial(cy::ecs::Stage::Simulation).has_value());

    CY_CHECK_EQ(world.entity_count(), 2u);
    const cy::ecs::Entity created_first = world.entities().at(0);
    const cy::ecs::Entity created_second = world.entities().at(1);
    // 2.0 is alpha's mark: the entity created first belongs to the system whose name sorts first.
    CY_CHECK_EQ(
        cy::ecs::test::value_of<cy::ecs::test::Position>(world, created_first, ids->position).x,
        2.0F);
    CY_CHECK_EQ(
        cy::ecs::test::value_of<cy::ecs::test::Position>(world, created_second, ids->position).x,
        1.0F);

    // And the key itself, so a failure says whether the ranking or the flush is at fault.
    const cy::ecs::CommandBuffer* zulu_commands = schedule.commands(cy::ecs::Stage::Simulation, 0);
    const cy::ecs::CommandBuffer* alpha_commands = schedule.commands(cy::ecs::Stage::Simulation, 1);
    CY_REQUIRE(zulu_commands != nullptr);
    CY_REQUIRE(alpha_commands != nullptr);
    CY_CHECK_EQ(alpha_commands->system_order(), 0u);
    CY_CHECK_EQ(zulu_commands->system_order(), 1u);
}

CY_TEST_CASE("two systems iterating one world in parallel leave it not iterating") {
    // REGRESSION. `World::iterating_` was a plain counter. Two systems of one batch iterate
    // concurrently by construction — each over its own query, over this one world — so a lost
    // increment or decrement left the world either permanently "iterating", which makes the stage
    // refuse its own flush with Unavailable, or momentarily not, which is worse because a
    // structural change would then be admitted into a chunk another system is walking.
    //
    // It reproduced roughly once in eight runs of the case above before the counter was made
    // atomic. Sixty-four stages of two parallel iterators is what it took to see it reliably.
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    const cy::ecs::ComponentTypeId set[] = {ids->position, ids->velocity};
    cy::Array<cy::ecs::Entity> created(allocator());
    CY_REQUIRE(world.create_many(2048, cy::Span<const cy::ecs::ComponentTypeId>(set, 2), created)
                   .has_value());

    cy::ecs::QueryDesc writes_position(allocator());
    CY_REQUIRE(writes_position.write(ids->position).has_value());
    cy::ecs::Query position_query(world, std::move(writes_position));

    cy::ecs::QueryDesc writes_velocity(allocator());
    CY_REQUIRE(writes_velocity.write(ids->velocity).has_value());
    cy::ecs::Query velocity_query(world, std::move(writes_velocity));

    struct WalkState {
        cy::ecs::Query* query = nullptr;
    };
    static const auto walk = [](const cy::ecs::SystemContext& context) noexcept {
        auto* state = static_cast<WalkState*>(context.user);
        (void)state->query->for_each_chunk([](cy::ecs::QueryChunk&) {});
    };

    WalkState first_state{&position_query};
    WalkState second_state{&velocity_query};

    cy::ecs::Schedule schedule(world);
    cy::ecs::SystemDesc first;
    first.name = "walk-position";
    first.body = walk;
    first.user = &first_state;
    first.access = position_query.desc().access();

    cy::ecs::SystemDesc second;
    second.name = "walk-velocity";
    second.body = walk;
    second.user = &second_state;
    second.access = velocity_query.desc().access();

    CY_REQUIRE(schedule.add(cy::ecs::Stage::Simulation, first).has_value());
    CY_REQUIRE(schedule.add(cy::ecs::Stage::Simulation, second).has_value());
    CY_REQUIRE(schedule.build().has_value());
    // They write different components, so they are in one batch and really do run together.
    CY_REQUIRE_EQ(schedule.batch_count(cy::ecs::Stage::Simulation), 1u);

    cy::jobs::JobSystemConfig config;
    config.worker_count = 4;
    cy::jobs::JobSystem jobs;
    CY_REQUIRE(jobs.start(config).has_value());
    cy::u32 refused = 0;
    for (cy::u32 tick = 0; tick < 64; ++tick) {
        if (!schedule.run(cy::ecs::Stage::Simulation, jobs)) {
            ++refused;
        }
        if (world.iterating()) {
            ++refused;
        }
    }
    jobs.shutdown();

    CY_CHECK_EQ(refused, 0u);
    CY_CHECK_FALSE(world.iterating());
    CY_CHECK_EQ(world.refused_during_iteration(), 0u);
}
