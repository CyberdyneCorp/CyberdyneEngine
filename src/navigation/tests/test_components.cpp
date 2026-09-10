// The navigation components and the bulk pass over them. M8.b task 6.2.

#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/components.h>
#include <cy/test/test.h>

#include "nav_fixture.h"

using namespace cy;
using namespace cy::navigation;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// `ecs::World` is neither copyable nor movable, so it is built in place and this returns nothing.
void start(ecs::World& world) noexcept {
    CY_REQUIRE(world.initialize().has_value());
}

[[nodiscard]] ecs::WorldConfig config() noexcept {
    ecs::WorldConfig out;
    out.name = "navigation.components";
    return out;
}

[[nodiscard]] Entity spawn_agent(ecs::World& world, const NavComponents& ids, Vec3 position,
                                 Vec3 target) noexcept {
    const ComponentTypeId components[1] = {ids.agent};
    const Expected<Entity, Error> entity = world.create({components, 1});
    CY_REQUIRE(entity.has_value());
    NavAgent agent;
    agent.position = position;
    agent.target = target;
    CY_REQUIRE(world.set(*entity, ids.agent, agent).has_value());
    return *entity;
}

}  // namespace

CY_TEST_CASE("the five components register in a fixed order and a second call binds the same ids") {
    ecs::World world(allocator(), config());
    start(world);
    const Expected<NavComponents, Error> first = NavComponents::register_all(world);
    CY_REQUIRE(first.has_value());
    CY_CHECK(first->registered());

    const Expected<NavComponents, Error> second = NavComponents::register_all(world);
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(first->surface, second->surface);
    CY_CHECK_EQ(first->agent, second->agent);
    CY_CHECK_EQ(first->obstacle, second->obstacle);
    CY_CHECK_EQ(first->link, second->link);
    CY_CHECK_EQ(first->area, second->area);
    // The order is the id order: it is what a serialized descriptor table records.
    CY_CHECK_LT(first->surface, first->agent);
    CY_CHECK_LT(first->agent, first->obstacle);
    CY_CHECK_LT(first->obstacle, first->link);
    CY_CHECK_LT(first->link, first->area);
}

CY_TEST_CASE("a pass over agents issues one query each and rate-limits the next") {
    ecs::World world(allocator(), config());
    start(world);
    const Expected<NavComponents, Error> ids = NavComponents::register_all(world);
    CY_REQUIRE(ids.has_value());
    NavMesh mesh = testing::single_tile_mesh(allocator(), 16.0F, 8);
    PathQueue queue(allocator(), mesh, 1);

    for (u32 index = 0; index < 4; ++index) {
        (void)spawn_agent(world, *ids, Vec3{1.0F, 0.0F, 1.0F + static_cast<f32>(index)},
                          Vec3{14.0F, 0.0F, 14.0F});
    }

    NavAgentReport report;
    CY_REQUIRE(update_agents(world, *ids, mesh, queue, 10, 30, report).has_value());
    CY_CHECK_EQ(report.agents, 4U);
    CY_CHECK_EQ(report.repaths_issued, 4U);
    CY_CHECK_EQ(queue.pending(), 4U);

    // Every agent is Computing, so a second pass issues nothing at all.
    CY_REQUIRE(update_agents(world, *ids, mesh, queue, 11, 30, report).has_value());
    CY_CHECK_EQ(report.repaths_issued, 0U);
}

CY_TEST_CASE("an agent already at its target arrives and raises its event") {
    ecs::World world(allocator(), config());
    start(world);
    const Expected<NavComponents, Error> ids = NavComponents::register_all(world);
    CY_REQUIRE(ids.has_value());
    NavMesh mesh = testing::single_tile_mesh(allocator(), 16.0F, 8);
    PathQueue queue(allocator(), mesh, 1);

    const Entity entity = spawn_agent(world, *ids, Vec3{8.0F, 0.0F, 8.0F}, Vec3{8.1F, 0.0F, 8.0F});
    NavAgentReport report;
    CY_REQUIRE(update_agents(world, *ids, mesh, queue, 5, 30, report).has_value());
    CY_CHECK_EQ(report.arrived, 1U);
    CY_CHECK_EQ(report.repaths_issued, 0U);

    const auto* agent = world.get<NavAgent>(entity, ids->agent);
    CY_REQUIRE(agent != nullptr);
    CY_CHECK_EQ(agent->status, NavPathStatus::Arrived);
    CY_CHECK(agent->event_pending);

    // The event is cleared by the next pass rather than by the reader, so it lasts exactly one
    // tick.
    CY_REQUIRE(update_agents(world, *ids, mesh, queue, 6, 30, report).has_value());
    CY_CHECK_FALSE(world.get<NavAgent>(entity, ids->agent)->event_pending);
}

CY_TEST_CASE("an agent whose target is off the mesh fails rather than waiting forever") {
    ecs::World world(allocator(), config());
    start(world);
    const Expected<NavComponents, Error> ids = NavComponents::register_all(world);
    CY_REQUIRE(ids.has_value());
    NavMesh mesh = testing::single_tile_mesh(allocator(), 16.0F, 8);
    PathQueue queue(allocator(), mesh, 1);

    const Entity entity =
        spawn_agent(world, *ids, Vec3{4.0F, 0.0F, 4.0F}, Vec3{400.0F, 0.0F, 400.0F});
    NavAgentReport report;
    CY_REQUIRE(update_agents(world, *ids, mesh, queue, 3, 30, report).has_value());
    CY_CHECK_EQ(report.failed, 1U);
    CY_CHECK_EQ(world.get<NavAgent>(entity, ids->agent)->status, NavPathStatus::Failed);
}

CY_TEST_CASE("a following agent repaths when the mesh version it was built against moves on") {
    ecs::World world(allocator(), config());
    start(world);
    const Expected<NavComponents, Error> ids = NavComponents::register_all(world);
    CY_REQUIRE(ids.has_value());
    NavMesh mesh = testing::single_tile_mesh(allocator(), 16.0F, 8);
    PathQueue queue(allocator(), mesh, 1);

    const Entity entity =
        spawn_agent(world, *ids, Vec3{2.0F, 0.0F, 2.0F}, Vec3{14.0F, 0.0F, 14.0F});
    auto* agent = world.get_mut<NavAgent>(entity, ids->agent);
    CY_REQUIRE(agent != nullptr);
    agent->status = NavPathStatus::Following;
    agent->corridor_version = mesh.version();

    NavAgentReport report;
    CY_REQUIRE(update_agents(world, *ids, mesh, queue, 20, 1, report).has_value());
    CY_CHECK_EQ(report.corridors_invalidated, 0U);
    CY_CHECK_EQ(report.repaths_issued, 0U);

    // A tile is rebuilt: the mesh version moves, and the next pass invalidates and repaths.
    CY_REQUIRE(
        mesh.add_tile(testing::grid_tile(allocator(), TileCoord{0, 0, 0}, 16.0F, 8)).has_value());
    CY_REQUIRE(update_agents(world, *ids, mesh, queue, 21, 1, report).has_value());
    CY_CHECK_EQ(report.corridors_invalidated, 1U);
    CY_CHECK_EQ(report.repaths_issued, 1U);
}

CY_TEST_CASE("a field-following agent issues no path query at all") {
    // `ai-system`: "WHEN an agent is demoted to Reduced THEN it SHALL follow a shared flow field
    // rather than computing an individual path." The saving is the query that is not issued.
    ecs::World world(allocator(), config());
    start(world);
    const Expected<NavComponents, Error> ids = NavComponents::register_all(world);
    CY_REQUIRE(ids.has_value());
    NavMesh mesh = testing::single_tile_mesh(allocator(), 16.0F, 8);
    PathQueue queue(allocator(), mesh, 1);

    const Entity entity =
        spawn_agent(world, *ids, Vec3{2.0F, 0.0F, 2.0F}, Vec3{14.0F, 0.0F, 14.0F});
    world.get_mut<NavAgent>(entity, ids->agent)->follows_field = true;

    NavAgentReport report;
    CY_REQUIRE(update_agents(world, *ids, mesh, queue, 7, 30, report).has_value());
    CY_CHECK_EQ(report.agents, 1U);
    CY_CHECK_EQ(report.repaths_issued, 0U);
    CY_CHECK_EQ(queue.pending(), 0U);
}

CY_TEST_CASE("a world with no navigation components registered is refused, not reported empty") {
    ecs::World world(allocator(), config());
    start(world);
    NavMesh mesh = testing::single_tile_mesh(allocator());
    PathQueue queue(allocator(), mesh, 1);
    NavAgentReport report;
    const NavComponents unregistered;
    const Status refused = update_agents(world, unregistered, mesh, queue, 0, 30, report);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);
}
