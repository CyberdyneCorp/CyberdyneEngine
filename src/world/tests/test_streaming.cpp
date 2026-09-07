// The streaming tick: channels, budgets, priorities, deadlines, eviction and diagnostics. Tasks
// 3.4 and 3.5.

#include <cy/test/test.h>

#include <cy/world/streaming.h>

#include "fixtures.h"

namespace {

cy::world::PartitionConfig grid_config() noexcept {
    return cy::world::uniform_grid_config(100.0f);
}

cy::world::WorldPosition at(cy::f64 x) noexcept {
    return cy::world::from_absolute(grid_config(), cy::world::WorldVec3d{x, 0.0, 50.0}, 0);
}

/// A row of `count` cells along x, each holding `rows` props.
cy::Status populate(cy::world::WorldStreaming& world, const cy::world::Partitioner& grid,
                    const cy::world::test::Components& ids, cy::i32 count, cy::u32 rows) noexcept {
    for (cy::i32 x = 0; x < count; ++x) {
        cy::world::CookedCell cell = cy::world::test::cook_props(
            cy::world::test::allocator(), grid, cy::world::CellCoord{x, 0, 0, 0}, rows, ids,
            (static_cast<cy::u64>(x) * 1000) + 1);
        if (cy::Status added = world.add_cell(std::move(cell)); !added) {
            return added;
        }
    }
    return cy::ok();
}

/// A budget nothing is refused under.
cy::world::StreamingBudget generous() noexcept {
    cy::world::StreamingBudget budget;
    budget.io_bytes_per_tick = 1ull << 30;
    budget.entity_memory_bytes = 1ull << 30;
    budget.activation_time_per_tick = 1'000'000'000;
    return budget;
}

}  // namespace

CY_TEST_CASE("residency and activation are separate: bytes resident with simulation off") {
    // The M6 exit criterion, from the world's side of it. A prefetch source buys RESIDENCY and asks
    // for no activation; the cell holds its bytes and the ECS world stays empty.
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::WorldStreaming world(cy::world::test::allocator(), grid, ecs);
    CY_REQUIRE(populate(world, grid, *ids, 4, 100).has_value());

    const auto prefetch = world.prefetch(at(50.0), 150.0f, cy::world::RequestClass::Background,
                                         cy::world::ChannelMask::all());
    CY_REQUIRE(prefetch.has_value());

    for (cy::u32 tick = 0; tick < 4; ++tick) {
        CY_REQUIRE(world.tick(generous()).has_value());
    }

    const cy::world::CellId here = grid.id_of(cy::world::CellCoord{0, 0, 0, 0});
    CY_CHECK(world.state_of(here) == cy::world::CellState::Resident);
    CY_CHECK_EQ(ecs.entity_count(), 0u);
    CY_CHECK_GT(world.stats().resident_io_bytes, 0u);
    CY_CHECK_EQ(world.stats().published_entities, 0u);

    // Now a source that DOES activate, over the same cells. Nothing else changes.
    cy::world::StreamingSource player;
    player.position = at(50.0);
    player.radius = 150.0f;
    player.activates = true;
    player.klass = cy::world::RequestClass::Gameplay;
    CY_REQUIRE(world.sources().add(player).has_value());
    for (cy::u32 tick = 0; tick < 4; ++tick) {
        CY_REQUIRE(world.tick(generous()).has_value());
    }
    CY_CHECK(world.state_of(here) == cy::world::CellState::Activated);
    CY_CHECK_GT(ecs.entity_count(), 0u);
}

CY_TEST_CASE("a channel added later streams for cells already resident, without reloading them") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::WorldStreaming world(cy::world::test::allocator(), grid, ecs);
    CY_REQUIRE(populate(world, grid, *ids, 2, 50).has_value());

    // A spectator: geometry and textures, no physics, no navigation, no AI.
    cy::world::ChannelMask visual;
    visual.set(cy::world::Channel::Entities);
    visual.set(cy::world::Channel::Geometry);
    visual.set(cy::world::Channel::Textures);

    cy::world::StreamingSource spectator;
    spectator.position = at(50.0);
    spectator.radius = 80.0f;
    spectator.channels = visual;
    spectator.activates = false;
    const auto id = world.sources().add(spectator);
    CY_REQUIRE(id.has_value());

    CY_REQUIRE(world.tick(generous()).has_value());
    const cy::world::CellId here = grid.id_of(cy::world::CellCoord{0, 0, 0, 0});
    cy::world::CellExplanation before = world.explain(here);
    CY_CHECK(before.state == cy::world::CellState::Resident);
    CY_CHECK(before.resident_channels.has(cy::world::Channel::Geometry));
    CY_CHECK_FALSE(before.resident_channels.has(cy::world::Channel::Physics));

    // The mask gains physics.
    cy::world::StreamingSource simulating = spectator;
    simulating.channels.set(cy::world::Channel::Physics);
    CY_REQUIRE(world.sources().update(*id, simulating).has_value());

    const auto report = world.tick(generous());
    CY_REQUIRE(report.has_value());
    CY_CHECK_GT(report->channel_deltas, 0u);
    // NOT reloaded: it was already resident and stayed so, and nothing was made resident afresh.
    CY_CHECK_EQ(report->cells_made_resident, 0u);
    const cy::world::CellExplanation after = world.explain(here);
    CY_CHECK(after.state == cy::world::CellState::Resident);
    CY_CHECK(after.resident_channels.has(cy::world::Channel::Physics));
    CY_CHECK(after.resident_channels.has(cy::world::Channel::Geometry));
}

CY_TEST_CASE("a dedicated server never requires the rendering channels") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::WorldStreaming world(cy::world::test::allocator(), grid, ecs);
    world.set_profile(cy::world::WorldProfile::DedicatedServer);
    CY_REQUIRE(populate(world, grid, *ids, 2, 50).has_value());

    cy::world::StreamingSource client;
    client.position = at(50.0);
    client.radius = 80.0f;
    client.channels = cy::world::ChannelMask::all();
    CY_REQUIRE(world.sources().add(client).has_value());
    CY_REQUIRE(world.tick(generous()).has_value());

    const cy::world::CellExplanation cell =
        world.explain(grid.id_of(cy::world::CellCoord{0, 0, 0, 0}));
    CY_CHECK(cell.required_channels.has(cy::world::Channel::Physics));
    CY_CHECK(cell.required_channels.has(cy::world::Channel::Navigation));
    // Even though the source asked for everything: the profile is what selects, and the cell's
    // identity did not change because of it.
    CY_CHECK_FALSE(cell.required_channels.has(cy::world::Channel::Geometry));
    CY_CHECK_FALSE(cell.required_channels.has(cy::world::Channel::Audio));
}

CY_TEST_CASE("urgent beats predicted, and the budget defers rather than stalls") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::WorldStreaming world(cy::world::test::allocator(), grid, ecs);
    CY_REQUIRE(populate(world, grid, *ids, 12, 200).has_value());

    // A background prefetch over the whole row, and a teleport at the far end.
    cy::world::StreamingSource background;
    background.position = at(50.0);
    background.radius = 1200.0f;
    background.klass = cy::world::RequestClass::Background;
    background.importance = 0.2f;
    background.activates = false;
    CY_REQUIRE(world.sources().add(background).has_value());

    cy::world::StreamingSource teleport;
    teleport.position = at(1050.0);
    teleport.radius = 60.0f;
    teleport.klass = cy::world::RequestClass::Critical;
    teleport.importance = 1.0f;
    CY_REQUIRE(world.sources().add(teleport).has_value());

    // A budget that fits a couple of cells, not twelve.
    cy::world::StreamingBudget tight = generous();
    tight.io_bytes_per_tick = 12000;

    const auto report = world.tick(tight);
    CY_REQUIRE(report.has_value());
    CY_CHECK_GT(report->deferred, 0u);
    CY_CHECK_LE(report->io_bytes_spent, tight.io_bytes_per_tick);

    // "WHEN a teleport request and a background prefetch compete for I/O, THEN the teleport SHALL
    // be serviced first and the prefetch deferred."
    const cy::world::CellId destination = grid.id_of(cy::world::CellCoord{10, 0, 0, 0});
    const cy::world::CellState reached = world.state_of(destination);
    const bool destination_is_here =
        reached == cy::world::CellState::Resident || reached == cy::world::CellState::Activated;
    CY_CHECK(destination_is_here);

    const cy::world::CellExplanation destination_state = world.explain(destination);
    CY_CHECK(destination_state.klass == cy::world::RequestClass::Critical);
    CY_CHECK_GT(destination_state.priority, 0.5f);
}

CY_TEST_CASE("activation is spread across ticks and never exceeds its budget") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::WorldStreaming world(cy::world::test::allocator(), grid, ecs);
    CY_REQUIRE(populate(world, grid, *ids, 6, 2000).has_value());

    cy::world::StreamingSource player;
    player.position = at(250.0);
    player.radius = 400.0f;
    player.activates = true;
    CY_REQUIRE(world.sources().add(player).has_value());

    cy::world::StreamingBudget budget = generous();
    budget.activation_time_per_tick = 300'000;  // 0.3 ms of modelled work

    cy::u32 ticks = 0;
    cy::u32 activated = 0;
    while (activated < 6 && ticks < 200) {
        const auto report = world.tick(budget);
        CY_REQUIRE(report.has_value());
        // "WHEN a large cell becomes ready, THEN its preparation SHALL be spread across frames
        // within the activation budget, and the frame SHALL not stall." One publication can carry
        // the tick past the budget; nothing may start a second one after that.
        CY_CHECK_LT(
            report->activation_time_spent,
            budget.activation_time_per_tick + cy::world::estimate_activation_time(2000, 0) + 1);
        activated += report->cells_activated;
        ++ticks;
    }
    CY_CHECK_EQ(activated, 6u);
    CY_CHECK_GT(ticks, 1u);
    CY_CHECK_EQ(ecs.entity_count(), 12000u);
}

CY_TEST_CASE("cells stream out behind a moving source, and back in when it returns") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::WorldStreaming world(cy::world::test::allocator(), grid, ecs);
    CY_REQUIRE(populate(world, grid, *ids, 20, 100).has_value());

    cy::world::StreamingSource player;
    player.position = at(50.0);
    player.radius = 120.0f;
    player.activates = true;
    const auto id = world.sources().add(player);
    CY_REQUIRE(id.has_value());

    for (cy::u32 tick = 0; tick < 5; ++tick) {
        CY_REQUIRE(world.tick(generous()).has_value());
    }
    const cy::world::CellId start = grid.id_of(cy::world::CellCoord{0, 0, 0, 0});
    const cy::world::CellId end = grid.id_of(cy::world::CellCoord{18, 0, 0, 0});
    CY_CHECK(world.state_of(start) == cy::world::CellState::Activated);
    // Two kilometres away and never requested: its metadata is known and nothing else is.
    CY_CHECK(world.state_of(end) == cy::world::CellState::Metadata);
    const cy::u32 near_start = ecs.entity_count();
    CY_CHECK_GT(near_start, 0u);

    CY_REQUIRE(world.sources().move_to(*id, at(1850.0), cy::Vec3{}).has_value());
    for (cy::u32 tick = 0; tick < 5; ++tick) {
        CY_REQUIRE(world.tick(generous()).has_value());
    }
    CY_CHECK(world.state_of(end) == cy::world::CellState::Activated);
    CY_CHECK(world.state_of(start) != cy::world::CellState::Activated);

    CY_REQUIRE(world.sources().move_to(*id, at(50.0), cy::Vec3{}).has_value());
    for (cy::u32 tick = 0; tick < 5; ++tick) {
        CY_REQUIRE(world.tick(generous()).has_value());
    }
    CY_CHECK(world.state_of(start) == cy::world::CellState::Activated);
    CY_CHECK_EQ(ecs.entity_count(), near_start);
}

CY_TEST_CASE("memory pressure evicts the lowest priority, and reports what it cannot free") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::WorldStreaming world(cy::world::test::allocator(), grid, ecs);
    CY_REQUIRE(populate(world, grid, *ids, 8, 500).has_value());

    cy::world::StreamingSource wide;
    wide.position = at(400.0);
    wide.radius = 500.0f;
    wide.activates = true;
    const auto id = world.sources().add(wide);
    CY_REQUIRE(id.has_value());
    for (cy::u32 tick = 0; tick < 12; ++tick) {
        CY_REQUIRE(world.tick(generous()).has_value());
    }
    CY_CHECK_GT(world.stats().staged_bytes, 0u);

    // Everything stops being required, and the budget is squeezed. The evictable cells go.
    CY_REQUIRE(world.sources().remove(*id).has_value());
    cy::world::StreamingBudget squeezed = generous();
    squeezed.entity_memory_bytes = 1024;

    cy::u32 evicted = 0;
    for (cy::u32 tick = 0; tick < 12; ++tick) {
        const auto report = world.tick(squeezed);
        CY_REQUIRE(report.has_value());
        evicted += report->cells_evicted;
    }
    CY_CHECK_GT(evicted, 0u);
    CY_CHECK_EQ(world.stats().staged_bytes, 0u);
    CY_CHECK_EQ(ecs.entity_count(), 0u);

    // A budget nothing required can satisfy is REPORTED rather than silently exceeded.
    cy::world::StreamingSource pinning;
    pinning.position = at(400.0);
    pinning.radius = 500.0f;
    pinning.activates = true;
    CY_REQUIRE(world.sources().add(pinning).has_value());
    cy::u64 shortfall = 0;
    for (cy::u32 tick = 0; tick < 12; ++tick) {
        const auto report = world.tick(squeezed);
        CY_REQUIRE(report.has_value());
        shortfall = report->memory_shortfall;
    }
    CY_CHECK_GT(shortfall, 0u);
}

CY_TEST_CASE("a request made while consuming an event is queued, not processed re-entrantly") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::WorldStreaming world(cy::world::test::allocator(), grid, ecs);
    CY_REQUIRE(populate(world, grid, *ids, 6, 40).has_value());

    const auto consumer = world.events().add_consumer("gameplay", 0);
    CY_REQUIRE(consumer.has_value());

    cy::world::StreamingSource player;
    player.position = at(50.0);
    player.radius = 60.0f;
    player.activates = true;
    CY_REQUIRE(world.sources().add(player).has_value());
    for (cy::u32 tick = 0; tick < 3; ++tick) {
        CY_REQUIRE(world.tick(generous()).has_value());
    }

    const cy::world::CellId neighbour = grid.id_of(cy::world::CellCoord{4, 0, 0, 0});
    CY_CHECK(world.state_of(neighbour) == cy::world::CellState::Metadata);

    // A consumer reacts to activation by asking for another cell. The state does not change while
    // it is consuming — there is no callback for the request to be re-entrant from.
    cy::Array<cy::world::CellEvent> events(cy::world::test::allocator());
    CY_REQUIRE(world.events().drain(*consumer, events).has_value());
    CY_CHECK_GT(events.size(), 0u);
    CY_REQUIRE(world.request(neighbour, cy::world::ChannelMask::all(), true).has_value());
    CY_CHECK(world.state_of(neighbour) == cy::world::CellState::Metadata);

    // It is taken up at the start of the next tick.
    for (cy::u32 tick = 0; tick < 3; ++tick) {
        CY_REQUIRE(world.tick(generous()).has_value());
    }
    CY_CHECK(world.state_of(neighbour) == cy::world::CellState::Activated);
}

CY_TEST_CASE("the debugger answers why a cell is loaded and why one is not") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::WorldStreaming world(cy::world::test::allocator(), grid, ecs);
    CY_REQUIRE(populate(world, grid, *ids, 6, 40).has_value());

    cy::world::StreamingSource player;
    player.position = at(50.0);
    player.radius = 60.0f;
    player.activates = true;
    const auto id = world.sources().add(player);
    CY_REQUIRE(id.has_value());
    for (cy::u32 tick = 0; tick < 3; ++tick) {
        CY_REQUIRE(world.tick(generous()).has_value());
    }

    // "WHEN a developer selects a resident cell, THEN the debugger SHALL name the streaming sources
    // requiring it and the priority that caused it to be requested."
    const cy::world::CellExplanation loaded =
        world.explain(grid.id_of(cy::world::CellCoord{0, 0, 0, 0}));
    CY_CHECK(loaded.known);
    CY_CHECK(loaded.requested);
    CY_CHECK_EQ(loaded.requiring_sources, 1u);
    CY_CHECK_EQ(loaded.leading_source, *id);
    CY_CHECK_GT(loaded.priority, 0.0f);
    CY_CHECK(loaded.state == cy::world::CellState::Activated);

    // "WHEN expected content is missing, THEN the debugger SHALL state whether it is unrequested,
    // queued behind higher priority, budget-blocked, or failed."
    const cy::world::CellExplanation missing =
        world.explain(grid.id_of(cy::world::CellCoord{5, 0, 0, 0}));
    CY_CHECK(missing.known);
    CY_CHECK_FALSE(missing.requested);
    CY_CHECK(cy::world::test::same_text(missing.blocking, "unrequested"));

    const cy::world::CellExplanation nonexistent = world.explain(cy::world::CellId{12345});
    CY_CHECK_FALSE(nonexistent.known);
    CY_CHECK(
        cy::world::test::same_text(nonexistent.blocking, "this cell is not in the world index"));
}

CY_TEST_CASE("walking into one cell that pulls a chain is reported with the chain") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::WorldStreaming world(cy::world::test::allocator(), grid, ecs);

    // Four cells, each hard-referencing the next: activating the first pulls the rest.
    for (cy::i32 x = 0; x < 4; ++x) {
        const cy::world::CellCoord coord{x, 0, 0, 0};
        cy::world::CellBuilder builder(cy::world::test::allocator(), grid.id_of(coord), coord);
        const cy::ecs::ComponentTypeId components[] = {ids->placement};
        cy::world::test::Placement placement;
        const void* values[] = {&placement};
        const cy::u32 sizes[] = {static_cast<cy::u32>(sizeof(placement))};
        CY_REQUIRE(builder
                       .add_entity(cy::world::PersistentId{static_cast<cy::u64>(x) + 1},
                                   cy::world::kDefaultLayer, components, values, sizes)
                       .has_value());
        if (x < 3) {
            CY_REQUIRE(builder
                           .add_reference(cy::world::PersistentReference{
                               cy::world::PersistentId{static_cast<cy::u64>(x) + 1},
                               cy::world::PersistentId{static_cast<cy::u64>(x) + 2},
                               grid.id_of(cy::world::CellCoord{x + 1, 0, 0, 0}),
                               cy::world::ReferencePolicy::RequireLoaded})
                           .has_value());
        }
        CY_REQUIRE(world.add_cell(builder.finish()).has_value());
    }

    cy::Array<cy::world::DependencyReport> reports(cy::world::test::allocator());
    CY_REQUIRE(world.dependency_report(2, reports).has_value());
    CY_REQUIRE_EQ(reports.size(), 3u);

    const cy::world::DependencyReport* first = nullptr;
    for (const cy::world::DependencyReport& report : reports.span()) {
        if (report.cell == grid.id_of(cy::world::CellCoord{0, 0, 0, 0})) {
            first = &report;
        }
    }
    CY_REQUIRE(first != nullptr);
    CY_CHECK_EQ(first->forced_cells, 3u);
    CY_CHECK(first->over_threshold);
    CY_CHECK(first->first_link == grid.id_of(cy::world::CellCoord{1, 0, 0, 0}));

    // And at runtime the closure is required: requesting the first makes all four resident.
    cy::world::StreamingSource player;
    player.position = at(50.0);
    player.radius = 40.0f;
    player.activates = false;
    CY_REQUIRE(world.sources().add(player).has_value());
    for (cy::u32 tick = 0; tick < 3; ++tick) {
        CY_REQUIRE(world.tick(generous()).has_value());
    }
    for (cy::i32 x = 0; x < 4; ++x) {
        CY_CHECK(world.state_of(grid.id_of(cy::world::CellCoord{x, 0, 0, 0})) ==
                 cy::world::CellState::Resident);
    }
}

CY_TEST_CASE("a scenario switch is one operation over every published cell") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::WorldStreaming world(cy::world::test::allocator(), grid, ecs);

    const cy::world::LayerId intact{300};
    const cy::world::LayerId ruined{301};
    CY_REQUIRE(world.layers().declare(intact, cy::world::LayerKind::Runtime, "city").has_value());
    CY_REQUIRE(world.layers().declare(ruined, cy::world::LayerKind::Scenario, "ruins").has_value());

    for (cy::i32 x = 0; x < 4; ++x) {
        const cy::world::CellCoord coord{x, 0, 0, 0};
        cy::world::CellBuilder builder(cy::world::test::allocator(), grid.id_of(coord), coord);
        const cy::ecs::ComponentTypeId components[] = {ids->placement};
        cy::world::test::Placement placement;
        const void* values[] = {&placement};
        const cy::u32 sizes[] = {static_cast<cy::u32>(sizeof(placement))};
        for (cy::u32 row = 0; row < 250; ++row) {
            const cy::u64 base = (static_cast<cy::u64>(x) * 1000) + row + 1;
            CY_REQUIRE(
                builder.add_entity(cy::world::PersistentId{base}, intact, components, values, sizes)
                    .has_value());
            CY_REQUIRE(builder
                           .add_entity(cy::world::PersistentId{base + 500}, ruined, components,
                                       values, sizes)
                           .has_value());
        }
        CY_REQUIRE(world.add_cell(builder.finish()).has_value());
    }

    cy::world::StreamingSource player;
    player.position = at(200.0);
    player.radius = 500.0f;
    player.activates = true;
    CY_REQUIRE(world.sources().add(player).has_value());
    for (cy::u32 tick = 0; tick < 6; ++tick) {
        CY_REQUIRE(world.tick(generous()).has_value());
    }
    CY_CHECK_EQ(ecs.entity_count(), 1000u);

    CY_REQUIRE(world.set_layer_state(ruined, cy::world::LayerState::Activated).has_value());
    CY_REQUIRE(world.set_layer_state(intact, cy::world::LayerState::Unloaded).has_value());
    CY_CHECK_EQ(ecs.entity_count(), 1000u);

    // Entities outside the switched layers are unaffected — here, the switch is exact: exactly the
    // ruined layer is present.
    CY_CHECK(world.layers().state_of(ruined) == cy::world::LayerState::Activated);
    CY_CHECK(world.layers().state_of(intact) == cy::world::LayerState::Unloaded);
}

CY_TEST_CASE("the HLOD proxy is showing exactly while its cells are not published") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::WorldStreaming world(cy::world::test::allocator(), grid, ecs);
    CY_REQUIRE(populate(world, grid, *ids, 3, 100).has_value());

    const cy::world::CellId district = grid.id_of(cy::world::CellCoord{0, 0, 0, 0});
    CY_REQUIRE(world.hlod().declare(district, cy::AssetId(7, 7), 0).has_value());

    CY_REQUIRE(world.tick(generous()).has_value());
    CY_CHECK(world.hlod().is_visible(district));

    cy::world::StreamingSource player;
    player.position = at(50.0);
    player.radius = 40.0f;
    player.activates = true;
    CY_REQUIRE(world.sources().add(player).has_value());
    for (cy::u32 tick = 0; tick < 4; ++tick) {
        CY_REQUIRE(world.tick(generous()).has_value());
        // At no point are both absent: the proxy is visible in every tick the cell is not.
        const bool activated = world.state_of(district) == cy::world::CellState::Activated;
        const bool something_is_showing = activated || world.hlod().is_visible(district);
        CY_CHECK(something_is_showing);
    }
    CY_CHECK(world.state_of(district) == cy::world::CellState::Activated);
    CY_CHECK_FALSE(world.hlod().is_visible(district));
}

CY_TEST_CASE("a world is torn down mid-activation without leaving entities behind") {
    // RULE 4 at the level of the whole subsystem: the world is destroyed while cells are in every
    // phase at once — some preparing, some published — and the ECS world it published into outlives
    // it and must be left clean.
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());
    cy::world::HierarchicalGrid grid(grid_config());

    for (cy::u32 ticks = 0; ticks < 12; ++ticks) {
        cy::world::WorldStreaming world(cy::world::test::allocator(), grid, ecs);
        CY_REQUIRE(populate(world, grid, *ids, 8, 400).has_value());

        cy::world::StreamingSource player;
        player.position = at(400.0);
        player.radius = 500.0f;
        player.activates = true;
        CY_REQUIRE(world.sources().add(player).has_value());

        cy::world::StreamingBudget budget = generous();
        // Tight enough that after a couple of ticks some cells are published, some are mid
        // preparation and some have not started.
        budget.activation_time_per_tick = 150'000;
        for (cy::u32 tick = 0; tick < ticks; ++tick) {
            CY_REQUIRE(world.tick(budget).has_value());
        }
        // Destroyed here, at whatever mixture of phases this iteration reached.
    }

    // Every entity every one of those worlds published has been withdrawn.
    CY_CHECK_EQ(ecs.entity_count(), 0u);
    CY_CHECK_EQ(ecs.refused_during_iteration(), 0u);
}
