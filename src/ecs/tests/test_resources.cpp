// Resources and singletons. Task 2.7.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/demo/types.h>
#include <cy/core/reflect/demo/types.reflect.h>
#include <cy/ecs/system.h>
#include <cy/ecs/world.h>

#include "fixtures.h"

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Ecs);
}

struct SimulationClock {
    cy::u64 tick = 0;
    cy::f64 seconds = 0.0;
};

}  // namespace

CY_TEST_CASE("a resource is declared once, zeroed, and read back by id") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());

    const auto clock = world.resources().declare("SimulationClock", sizeof(SimulationClock),
                                                 alignof(SimulationClock));
    CY_REQUIRE(clock.has_value());

    // Zeroed rather than left as whatever the allocator returned: a system reading a resource at
    // startup is normal, and it must read a defined value.
    const auto* initial = world.resources().get<SimulationClock>(*clock);
    CY_REQUIRE(initial != nullptr);
    CY_CHECK_EQ(initial->tick, 0u);

    CY_REQUIRE(world.resources().set(*clock, SimulationClock{7, 0.25}).has_value());
    const auto* value = world.resources().get<SimulationClock>(*clock);
    CY_REQUIRE(value != nullptr);
    CY_CHECK_EQ(value->tick, 7u);
    CY_CHECK_EQ(value->seconds, 0.25);

    // Idempotent: two subsystems that both need the clock both get the same id.
    const auto again = world.resources().declare("SimulationClock", sizeof(SimulationClock),
                                                 alignof(SimulationClock));
    CY_REQUIRE(again.has_value());
    CY_CHECK_EQ(*again, *clock);
    CY_CHECK_EQ(world.resources().find("SimulationClock"), *clock);
    CY_CHECK_EQ(world.resources().find("nothing"), cy::ecs::kInvalidResource);
}

CY_TEST_CASE("a reflected type can be a resource and is found by its manifest identifier") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());

    const auto health = world.resources().declare<cy::demo::Health>();
    CY_REQUIRE(health.has_value());
    CY_CHECK_EQ(world.resources().find(cy::reflect::type_id_of<cy::demo::Health>()), *health);
    CY_CHECK_EQ(world.resources().value_size(*health),
                static_cast<cy::u32>(sizeof(cy::demo::Health)));
}

CY_TEST_CASE("two systems writing one resource are serialised by the scheduler") {
    cy::ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const auto ids = cy::ecs::test::register_all(world);
    CY_REQUIRE(ids.has_value());

    const auto clock = world.resources().declare("SimulationClock", sizeof(SimulationClock),
                                                 alignof(SimulationClock));
    CY_REQUIRE(clock.has_value());

    cy::ecs::Schedule schedule(world);

    cy::ecs::SystemDesc first;
    first.name = "advance-clock";
    first.body = [](const cy::ecs::SystemContext&) noexcept {};
    CY_REQUIRE(first.access.resource_write(*clock).has_value());

    cy::ecs::SystemDesc second;
    second.name = "publish-clock";
    second.body = [](const cy::ecs::SystemContext&) noexcept {};
    CY_REQUIRE(second.access.resource_write(*clock).has_value());

    cy::ecs::SystemDesc unrelated;
    unrelated.name = "move";
    unrelated.body = [](const cy::ecs::SystemContext&) noexcept {};
    CY_REQUIRE(unrelated.access.write(ids->position).has_value());

    const auto first_id = schedule.add(cy::ecs::Stage::Simulation, first);
    const auto second_id = schedule.add(cy::ecs::Stage::Simulation, second);
    const auto unrelated_id = schedule.add(cy::ecs::Stage::Simulation, unrelated);
    CY_REQUIRE(first_id.has_value());
    CY_REQUIRE(second_id.has_value());
    CY_REQUIRE(unrelated_id.has_value());
    CY_REQUIRE(schedule.build().has_value());

    // A resource participates in conflict detection exactly as a component does: the two writers
    // are ordered, and the system that touches neither runs beside them.
    CY_CHECK(schedule.ordered_before(cy::ecs::Stage::Simulation, *first_id, *second_id));
    CY_CHECK_FALSE(schedule.ordered_before(cy::ecs::Stage::Simulation, *first_id, *unrelated_id));
    cy::jobs::AccessConflict conflict;
    CY_CHECK(schedule.conflicts(cy::ecs::Stage::Simulation, *first_id, *second_id, conflict));
    CY_CHECK_EQ(conflict.domain, cy::jobs::AccessDomain::Resource);
    CY_CHECK_EQ(conflict.id, *clock);
}
