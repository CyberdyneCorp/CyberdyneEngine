// Streaming sources: shapes, prediction, combination and the central priority. Task 3.3.

#include <cy/test/test.h>

#include <cy/world/sources.h>

#include "fixtures.h"

namespace {

cy::world::PartitionConfig grid_config() noexcept {
    cy::world::PartitionConfig config;
    config.base_cell_size = 100.0f;
    config.levels = 2;
    config.level_ratio = 4;
    return config;
}

cy::world::WorldPosition at(cy::f64 x, cy::f64 z) noexcept {
    return cy::world::from_absolute(grid_config(), cy::world::WorldVec3d{x, 0.0, z}, 0);
}

const cy::world::CellRequirement* requirement_for(
    const cy::Array<cy::world::CellRequirement>& requirements, cy::world::CellId cell) noexcept {
    for (const cy::world::CellRequirement& requirement : requirements.span()) {
        if (requirement.cell == cell) {
            return &requirement;
        }
    }
    return nullptr;
}

}  // namespace

CY_TEST_CASE("a sphere source requires the cells within its radius and no others") {
    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::SourceRegistry sources(cy::world::test::allocator());

    cy::world::StreamingSource camera;
    camera.shape = cy::world::SourceShape::Sphere;
    camera.position = at(50.0, 50.0);
    camera.radius = 120.0f;
    camera.level_span = 1;
    CY_REQUIRE(sources.add(camera).has_value());

    cy::Array<cy::world::CellRequirement> requirements(cy::world::test::allocator());
    CY_REQUIRE(sources.require_all(grid, requirements).has_value());

    CY_CHECK(requirement_for(requirements, grid.id_of(cy::world::CellCoord{0, 0, 0, 0})) !=
             nullptr);
    CY_CHECK(requirement_for(requirements, grid.id_of(cy::world::CellCoord{1, 0, 0, 0})) !=
             nullptr);
    // Four cells away in x is 350 m from the source at its nearest face — well outside 120 m.
    CY_CHECK(requirement_for(requirements, grid.id_of(cy::world::CellCoord{4, 0, 0, 0})) ==
             nullptr);

    // The result is one canonical set in one canonical order, so two machines planning the same
    // frame agree.
    for (cy::usize index = 1; index < requirements.size(); ++index) {
        CY_CHECK(requirements[index - 1].cell < requirements[index].cell);
    }
}

CY_TEST_CASE("the camera is not the only source, and sources combine") {
    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::SourceRegistry sources(cy::world::test::allocator());

    // "WHEN a strategy camera is far from the units it commands, THEN both the camera and the units
    // SHALL be able to register sources, and content around BOTH shall stream."
    cy::world::StreamingSource camera;
    camera.position = at(0.0, 0.0);
    camera.radius = 60.0f;
    CY_REQUIRE(sources.add(camera).has_value());

    cy::world::StreamingSource units;
    units.position = at(2000.0, 0.0);
    units.radius = 60.0f;
    CY_REQUIRE(sources.add(units).has_value());

    cy::Array<cy::world::CellRequirement> requirements(cy::world::test::allocator());
    CY_REQUIRE(sources.require_all(grid, requirements).has_value());

    CY_CHECK(requirement_for(requirements, grid.id_of(cy::world::CellCoord{0, 0, 0, 0})) !=
             nullptr);
    CY_CHECK(requirement_for(requirements, grid.id_of(cy::world::CellCoord{20, 0, 0, 0})) !=
             nullptr);
}

CY_TEST_CASE("two sources over one cell merge: the union of channels, the most urgent class") {
    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::SourceRegistry sources(cy::world::test::allocator());

    cy::world::ChannelMask visual;
    visual.set(cy::world::Channel::Geometry);
    cy::world::ChannelMask simulated;
    simulated.set(cy::world::Channel::Physics);

    cy::world::StreamingSource spectator;
    spectator.position = at(50.0, 50.0);
    spectator.radius = 40.0f;
    spectator.channels = visual;
    spectator.klass = cy::world::RequestClass::Background;
    spectator.importance = 0.2f;
    spectator.activates = false;
    CY_REQUIRE(sources.add(spectator).has_value());

    cy::world::StreamingSource player;
    player.position = at(50.0, 50.0);
    player.radius = 40.0f;
    player.channels = simulated;
    player.klass = cy::world::RequestClass::Critical;
    player.importance = 1.0f;
    player.activates = true;
    CY_REQUIRE(sources.add(player).has_value());

    cy::Array<cy::world::CellRequirement> requirements(cy::world::test::allocator());
    CY_REQUIRE(sources.require_all(grid, requirements).has_value());

    const cy::world::CellRequirement* merged =
        requirement_for(requirements, grid.id_of(cy::world::CellCoord{0, 0, 0, 0}));
    CY_REQUIRE(merged != nullptr);
    CY_CHECK(merged->channels.has(cy::world::Channel::Geometry));
    CY_CHECK(merged->channels.has(cy::world::Channel::Physics));
    CY_CHECK(merged->klass == cy::world::RequestClass::Critical);
    CY_CHECK(merged->activate);
}

CY_TEST_CASE("a vehicle streams along its predicted route ahead of arrival") {
    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::SourceRegistry sources(cy::world::test::allocator());

    cy::world::StreamingSource vehicle;
    vehicle.position = at(50.0, 50.0);
    vehicle.radius = 40.0f;
    vehicle.velocity = cy::Vec3{60.0f, 0.0f, 0.0f};  // 60 m/s
    vehicle.prediction_horizon = 8.0f;               // 480 m ahead
    const auto id = sources.add(vehicle);
    CY_REQUIRE(id.has_value());

    cy::Array<cy::world::CellRequirement> predicted(cy::world::test::allocator());
    CY_REQUIRE(sources.require_all(grid, predicted).has_value());

    // Cells four cells ahead are requested BEFORE the vehicle enters them.
    const cy::world::CellRequirement* ahead =
        requirement_for(predicted, grid.id_of(cy::world::CellCoord{4, 0, 0, 0}));
    CY_REQUIRE(ahead != nullptr);
    CY_CHECK_GT(ahead->time_until_needed, 0);

    // Behind is not requested: prediction is directional, and a source that requested a sphere
    // around its whole horizon would be requesting twice what it needs.
    CY_CHECK(requirement_for(predicted, grid.id_of(cy::world::CellCoord{-4, 0, 0, 0})) == nullptr);

    // What it is standing in outranks what it will reach, because urgency is part of the priority.
    const cy::world::CellRequirement* here =
        requirement_for(predicted, grid.id_of(cy::world::CellCoord{0, 0, 0, 0}));
    CY_REQUIRE(here != nullptr);
    CY_CHECK_GT(here->priority, ahead->priority);

    // Turning prediction off leaves only what is near.
    cy::world::StreamingSource stopped = vehicle;
    stopped.prediction_horizon = 0.0f;
    CY_REQUIRE(sources.update(*id, stopped).has_value());
    cy::Array<cy::world::CellRequirement> immediate(cy::world::test::allocator());
    CY_REQUIRE(sources.require_all(grid, immediate).has_value());
    CY_CHECK(requirement_for(immediate, grid.id_of(cy::world::CellCoord{4, 0, 0, 0})) == nullptr);
}

CY_TEST_CASE("a path source requests its route, ordered by arc length") {
    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::SourceRegistry sources(cy::world::test::allocator());

    cy::Array<cy::world::WorldPosition> route(cy::world::test::allocator());
    CY_REQUIRE(route.push_back(at(300.0, 50.0)).has_value());
    CY_REQUIRE(route.push_back(at(600.0, 50.0)).has_value());
    CY_REQUIRE(route.push_back(at(900.0, 50.0)).has_value());

    cy::world::StreamingSource convoy;
    convoy.shape = cy::world::SourceShape::Path;
    convoy.position = at(50.0, 50.0);
    convoy.radius = 30.0f;
    convoy.velocity = cy::Vec3{30.0f, 0.0f, 0.0f};
    CY_REQUIRE(sources.add(convoy, route.span()).has_value());

    cy::Array<cy::world::CellRequirement> requirements(cy::world::test::allocator());
    CY_REQUIRE(sources.require_all(grid, requirements).has_value());

    const cy::world::CellRequirement* near_start =
        requirement_for(requirements, grid.id_of(cy::world::CellCoord{3, 0, 0, 0}));
    const cy::world::CellRequirement* far_end =
        requirement_for(requirements, grid.id_of(cy::world::CellCoord{9, 0, 0, 0}));
    CY_REQUIRE(near_start != nullptr);
    CY_REQUIRE(far_end != nullptr);
    // The end of the route is needed later than its start, and the priority reflects that.
    CY_CHECK_LT(near_start->time_until_needed, far_end->time_until_needed);
    CY_CHECK_GT(near_start->priority, far_end->priority);
}

CY_TEST_CASE("a cone source requires what it points at and not what is behind it") {
    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::SourceRegistry sources(cy::world::test::allocator());

    cy::world::StreamingSource spotlight;
    spotlight.shape = cy::world::SourceShape::Cone;
    spotlight.position = at(50.0, 50.0);
    spotlight.direction = cy::Vec3{1.0f, 0.0f, 0.0f};
    spotlight.radius = 400.0f;
    spotlight.cone_half_angle = 0.3f;
    CY_REQUIRE(sources.add(spotlight).has_value());

    cy::Array<cy::world::CellRequirement> requirements(cy::world::test::allocator());
    CY_REQUIRE(sources.require_all(grid, requirements).has_value());

    CY_CHECK(requirement_for(requirements, grid.id_of(cy::world::CellCoord{3, 0, 0, 0})) !=
             nullptr);
    CY_CHECK(requirement_for(requirements, grid.id_of(cy::world::CellCoord{-3, 0, 0, 0})) ==
             nullptr);
}

CY_TEST_CASE("a box source uses its extents per axis") {
    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::SourceRegistry sources(cy::world::test::allocator());

    cy::world::StreamingSource region;
    region.shape = cy::world::SourceShape::Box;
    region.position = at(50.0, 50.0);
    region.extent = cy::Vec3{350.0f, 50.0f, 50.0f};
    CY_REQUIRE(sources.add(region).has_value());

    cy::Array<cy::world::CellRequirement> requirements(cy::world::test::allocator());
    CY_REQUIRE(sources.require_all(grid, requirements).has_value());

    CY_CHECK(requirement_for(requirements, grid.id_of(cy::world::CellCoord{3, 0, 0, 0})) !=
             nullptr);
    // Wide in x, narrow in z: a sphere of the same reach would have taken this one.
    CY_CHECK(requirement_for(requirements, grid.id_of(cy::world::CellCoord{0, 0, 3, 0})) ==
             nullptr);
}

CY_TEST_CASE("a source is removed, and the order of the rest is unchanged") {
    cy::world::SourceRegistry sources(cy::world::test::allocator());
    cy::world::StreamingSource source;
    source.position = at(0.0, 0.0);

    const auto first = sources.add(source);
    const auto second = sources.add(source);
    const auto third = sources.add(source);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(third.has_value());

    CY_REQUIRE(sources.remove(*second).has_value());
    CY_CHECK_EQ(sources.size(), 2u);
    CY_CHECK_EQ(sources.entries()[0].id, *first);
    CY_CHECK_EQ(sources.entries()[1].id, *third);
    CY_CHECK(sources.find(*second) == nullptr);
    CY_CHECK_FALSE(sources.remove(*second).has_value());
}
