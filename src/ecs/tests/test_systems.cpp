// Systems, stages and the access declarations they are scheduled from. Task 2.5.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/query.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>

#include "fixtures.h"

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Ecs);
}

/// What a real system looks like: a query, its declaration taken from that query, and a body that
/// walks spans. `user` carries whatever state the system needs; nothing is captured, because a
/// system body is a plain function pointer the scheduler can hold.
struct MoveState {
    cy::ecs::Query* query = nullptr;
    cy::ecs::ComponentTypeId position = cy::ecs::kInvalidComponent;
    cy::ecs::ComponentTypeId velocity = cy::ecs::kInvalidComponent;
    cy::u64 entities_seen = 0;
    cy::u32 order = 0;
};

cy::u32 g_next_order = 1;

void move_body(const cy::ecs::SystemContext& context) noexcept {
    auto* state = static_cast<MoveState*>(context.user);
    state->order = g_next_order++;
    const cy::ecs::ComponentTypeId position = state->position;
    const cy::ecs::ComponentTypeId velocity = state->velocity;
    cy::u64 seen = 0;
    (void)state->query->for_each_chunk([&](cy::ecs::QueryChunk& chunk) {
        const cy::Span<cy::ecs::test::Position> positions =
            chunk.write<cy::ecs::test::Position>(position);
        const cy::Span<const cy::ecs::test::Velocity> velocities =
            chunk.read<cy::ecs::test::Velocity>(velocity);
        for (cy::u32 row = 0; row < chunk.count(); ++row) {
            positions[row].x += velocities[row].x;
        }
        seen += chunk.count();
    });
    state->entities_seen += seen;
}

void record_body(const cy::ecs::SystemContext& context) noexcept {
    auto* state = static_cast<MoveState*>(context.user);
    state->order = g_next_order++;
    // A system that spawns: it records rather than mutating, and the recording is applied at the
    // stage's flush point.
    auto created = context.commands->create();
    if (created) {
        (void)context.commands->add(*created, state->position,
                                    cy::ecs::test::Position{9.0F, 0.0F, 0.0F});
    }
}

}  // namespace

CY_TEST_CASE("the eight stages are in the order ecs-core fixes, split fixed and variable") {
    CY_CHECK_EQ(static_cast<cy::u8>(cy::ecs::Stage::PreSimulation), 0u);
    CY_CHECK_EQ(static_cast<cy::u8>(cy::ecs::Stage::Render), 7u);
    CY_CHECK(cy::ecs::stage_is_fixed_step(cy::ecs::Stage::PostSimulation));
    CY_CHECK_FALSE(cy::ecs::stage_is_fixed_step(cy::ecs::Stage::Frame));
    CY_CHECK(cy::ecs::test::same_text(cy::ecs::stage_name(cy::ecs::Stage::Animation), "Animation"));
}

CY_TEST_CASE("systems that do not conflict are batched together and conflicting ones are ordered") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    cy::ecs::Schedule schedule(world);

    cy::ecs::SystemDesc writer;
    writer.name = "write-position";
    writer.body = [](const cy::ecs::SystemContext&) noexcept {};
    CY_REQUIRE(writer.access.write(ids->position).has_value());

    cy::ecs::SystemDesc reader;
    reader.name = "read-position";
    reader.body = [](const cy::ecs::SystemContext&) noexcept {};
    CY_REQUIRE(reader.access.read(ids->position).has_value());

    cy::ecs::SystemDesc elsewhere;
    elsewhere.name = "write-velocity";
    elsewhere.body = [](const cy::ecs::SystemContext&) noexcept {};
    CY_REQUIRE(elsewhere.access.write(ids->velocity).has_value());

    const auto write_id = schedule.add(cy::ecs::Stage::Simulation, writer);
    const auto read_id = schedule.add(cy::ecs::Stage::Simulation, reader);
    const auto other_id = schedule.add(cy::ecs::Stage::Simulation, elsewhere);
    CY_REQUIRE(write_id.has_value());
    CY_REQUIRE(read_id.has_value());
    CY_REQUIRE(other_id.has_value());
    CY_REQUIRE(schedule.build().has_value());

    // Write vs Read on one component conflicts and is ordered by registration; a system touching
    // another component conflicts with neither and runs in the first batch beside the writer.
    CY_CHECK(schedule.ordered_before(cy::ecs::Stage::Simulation, *write_id, *read_id));
    CY_CHECK_FALSE(schedule.ordered_before(cy::ecs::Stage::Simulation, *write_id, *other_id));
    CY_CHECK_EQ(schedule.batch_count(cy::ecs::Stage::Simulation), 2u);
    CY_CHECK_EQ(schedule.batch_size(cy::ecs::Stage::Simulation, 0), 2u);
    CY_CHECK_EQ(schedule.system_count(cy::ecs::Stage::Simulation), 3u);
}

CY_TEST_CASE("an Exclude declaration conflicts with nothing") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    cy::ecs::Schedule schedule(world);

    cy::ecs::SystemDesc excluder;
    excluder.name = "skip-frozen";
    excluder.body = [](const cy::ecs::SystemContext&) noexcept {};
    CY_REQUIRE(excluder.access.exclude(ids->frozen).has_value());

    cy::ecs::SystemDesc freezer;
    freezer.name = "freeze";
    freezer.body = [](const cy::ecs::SystemContext&) noexcept {};
    CY_REQUIRE(freezer.access.write(ids->frozen).has_value());

    const auto first = schedule.add(cy::ecs::Stage::Simulation, excluder);
    const auto second = schedule.add(cy::ecs::Stage::Simulation, freezer);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(schedule.build().has_value());

    // An Exclude reads no component data: it is a filter over presence, and presence only changes
    // through a structural change, which is already deferred. So the two run together.
    cy::jobs::AccessConflict conflict;
    CY_CHECK_FALSE(schedule.conflicts(cy::ecs::Stage::Simulation, *first, *second, conflict));
    CY_CHECK_EQ(schedule.batch_count(cy::ecs::Stage::Simulation), 1u);
}

CY_TEST_CASE("an explicit ordering constraint is honoured and a cycle is refused") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    cy::ecs::Schedule schedule(world);

    cy::ecs::SystemDesc input;
    input.name = "input";
    input.body = [](const cy::ecs::SystemContext&) noexcept {};
    cy::ecs::SystemDesc movement;
    movement.name = "movement";
    movement.body = [](const cy::ecs::SystemContext&) noexcept {};
    CY_REQUIRE(movement.access.write(ids->position).has_value());

    const auto input_id = schedule.add(cy::ecs::Stage::Simulation, input);
    const auto movement_id = schedule.add(cy::ecs::Stage::Simulation, movement);
    CY_REQUIRE(input_id.has_value());
    CY_REQUIRE(movement_id.has_value());

    // No shared data at all, so the order has to be stated.
    CY_REQUIRE(schedule.order(cy::ecs::Stage::Simulation, *input_id, *movement_id).has_value());
    CY_CHECK(schedule.ordered_before(cy::ecs::Stage::Simulation, *input_id, *movement_id));

    // The reverse closes a cycle and is refused at registration, not discovered at run time.
    const auto cyclic = schedule.order(cy::ecs::Stage::Simulation, *movement_id, *input_id);
    CY_CHECK_FALSE(cyclic.has_value());
    CY_REQUIRE(schedule.build().has_value());
    CY_CHECK_EQ(schedule.batch_count(cy::ecs::Stage::Simulation), 2u);
}

CY_TEST_CASE("a duplicate system name and a contradictory declaration are both refused") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    cy::ecs::Schedule schedule(world);
    cy::ecs::SystemDesc desc;
    desc.name = "twice";
    desc.body = [](const cy::ecs::SystemContext&) noexcept {};
    CY_REQUIRE(schedule.add(cy::ecs::Stage::Simulation, desc).has_value());
    CY_CHECK_FALSE(schedule.add(cy::ecs::Stage::Simulation, desc).has_value());

    // Read and Write of one component in one declaration: there is no order in which it is
    // satisfiable, so AccessSet refuses it before a schedule ever sees it.
    cy::jobs::AccessSet contradictory;
    CY_REQUIRE(contradictory.read(ids->position).has_value());
    CY_CHECK_FALSE(contradictory.write(ids->position).has_value());

    // A body-less system cannot be scheduled.
    cy::ecs::SystemDesc empty;
    empty.name = "empty";
    CY_CHECK_FALSE(schedule.add(cy::ecs::Stage::Simulation, empty).has_value());
}

CY_TEST_CASE("a real system runs over its query and its spawns land at the stage flush") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    const cy::ecs::ComponentTypeId set[] = {ids->position, ids->velocity};
    cy::Array<cy::ecs::Entity> created(allocator());
    CY_REQUIRE(world.create_many(6, cy::Span<const cy::ecs::ComponentTypeId>(set, 2), created)
                   .has_value());
    for (const cy::ecs::Entity entity : created) {
        CY_REQUIRE(world.set(entity, ids->velocity, cy::ecs::test::Velocity{2.0F, 0.0F, 0.0F})
                       .has_value());
    }

    cy::ecs::QueryDesc desc(allocator());
    CY_REQUIRE(desc.write(ids->position).has_value());
    CY_REQUIRE(desc.read(ids->velocity).has_value());
    cy::ecs::Query query(world, std::move(desc));

    MoveState state;
    state.query = &query;
    state.position = ids->position;
    state.velocity = ids->velocity;

    MoveState spawner;
    spawner.position = ids->position;

    cy::ecs::Schedule schedule(world);
    cy::ecs::SystemDesc move;
    move.name = "move";
    move.body = &move_body;
    move.user = &state;
    // The declaration IS the query's. There is no second list to drift from the first.
    move.access = query.desc().access();

    cy::ecs::SystemDesc spawn;
    spawn.name = "spawn";
    spawn.body = &record_body;
    spawn.user = &spawner;
    CY_REQUIRE(spawn.access.write(ids->position).has_value());

    CY_REQUIRE(schedule.add(cy::ecs::Stage::Simulation, move).has_value());
    CY_REQUIRE(schedule.add(cy::ecs::Stage::Simulation, spawn).has_value());
    CY_REQUIRE(schedule.build().has_value());

    g_next_order = 1;
    const cy::u64 version_before = world.version();
    CY_REQUIRE(schedule.run_serial(cy::ecs::Stage::Simulation).has_value());

    // One version per stage; everything written during it carries that number.
    CY_CHECK_EQ(world.version(), version_before + 1);
    CY_CHECK_EQ(state.entities_seen, 6u);
    for (const cy::ecs::Entity entity : created) {
        CY_CHECK_EQ(
            cy::ecs::test::value_of<cy::ecs::test::Position>(world, entity, ids->position).x, 2.0F);
    }
    // The spawn was recorded during the stage and applied at its flush point.
    CY_CHECK_EQ(world.entity_count(), 7u);

    // Two conflicting writers of Position, so the order is the registration order and it is the
    // same on every run.
    CY_CHECK_EQ(state.order, 1u);
    CY_CHECK_EQ(spawner.order, 2u);

    const auto* profile = schedule.profile(cy::ecs::Stage::Simulation, 0);
    CY_REQUIRE(profile != nullptr);
    CY_CHECK_EQ(profile->runs, 1u);
    CY_CHECK(cy::ecs::test::same_text(profile->name, "move"));
}

CY_TEST_CASE("a stage that was extended must be rebuilt before it runs again") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());

    cy::ecs::Schedule schedule(world);
    cy::ecs::SystemDesc desc;
    desc.name = "one";
    desc.body = [](const cy::ecs::SystemContext&) noexcept {};
    CY_REQUIRE(schedule.add(cy::ecs::Stage::Frame, desc).has_value());

    // Running an unbuilt stage is refused rather than running a stale plan.
    CY_CHECK_FALSE(schedule.run_serial(cy::ecs::Stage::Frame).has_value());
    CY_REQUIRE(schedule.build().has_value());
    CY_REQUIRE(schedule.run_serial(cy::ecs::Stage::Frame).has_value());

    desc.name = "two";
    CY_REQUIRE(schedule.add(cy::ecs::Stage::Frame, desc).has_value());
    CY_CHECK_FALSE(schedule.run_serial(cy::ecs::Stage::Frame).has_value());
    CY_REQUIRE(schedule.build().has_value());
    CY_REQUIRE(schedule.run_serial(cy::ecs::Stage::Frame).has_value());

    // A stage with no systems is not an error; it simply does nothing.
    CY_CHECK(schedule.run_serial(cy::ecs::Stage::Render).has_value());
}
