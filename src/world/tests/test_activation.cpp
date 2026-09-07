// Staged, atomically-published activation, and its teardown mid-flight. Task 3.5.

#include <cy/test/test.h>

#include <cy/ecs/query.h>
#include <cy/world/activation.h>
#include <cy/world/overlay.h>

#include "fixtures.h"

namespace {

cy::world::PartitionConfig grid_config() noexcept {
    return cy::world::uniform_grid_config(128.0f);
}

/// A budget large enough that one call prepares everything.
constexpr cy::Nanoseconds kUnbounded = 1'000'000'000;

}  // namespace

CY_TEST_CASE("activation is a bulk copy: values land in ECS chunks with no per-entity work") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::LayerTable layers(cy::world::test::allocator());
    const cy::world::CookedCell cell = cy::world::test::cook_props(
        cy::world::test::allocator(), grid, cy::world::CellCoord{0, 0, 0, 0}, 1000, *ids, 1);

    cy::world::CellActivation activation(cy::world::test::allocator(), cell);
    const auto phase = activation.advance(cell, layers, nullptr, kUnbounded);
    CY_REQUIRE(phase.has_value());
    CY_CHECK(*phase == cy::world::StagingPhase::Ready);

    // Nothing is in the world yet. Preparation is PRIVATE.
    CY_CHECK_EQ(ecs.entity_count(), 0u);

    CY_REQUIRE(activation.publish(ecs, layers).has_value());
    CY_CHECK(activation.published());
    CY_CHECK_EQ(ecs.entity_count(), 1000u);
    CY_CHECK_EQ(activation.published_rows(), 1000u);

    // The columns arrived intact, in row order, so the fixup a cook records by row index works.
    const cy::Span<const cy::ecs::Entity> entities = activation.entities();
    CY_REQUIRE_EQ(entities.size(), 1000u);
    const auto* first = ecs.get<cy::world::test::Placement>(entities[0], ids->placement);
    const auto* last = ecs.get<cy::world::test::Placement>(entities[999], ids->placement);
    CY_REQUIRE(first != nullptr);
    CY_REQUIRE(last != nullptr);
    CY_CHECK_EQ(first->x, 0.0f);
    CY_CHECK_EQ(last->x, 999.0f);
    const auto* prop = ecs.get<cy::world::test::Prop>(entities[500], ids->prop);
    CY_REQUIRE(prop != nullptr);
    CY_CHECK_EQ(prop->kind, 500u);

    // One archetype for a thousand entities: the copy went into chunks of one layout, which is what
    // "its component columns SHALL match the runtime chunk layout" buys.
    CY_CHECK_EQ(ecs.archetypes().size(), 1u);
}

CY_TEST_CASE("preparation spans frames and nothing is observable until publication") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::LayerTable layers(cy::world::test::allocator());

    // Several blocks, so that a tight budget can only decode some of them per call.
    const cy::world::CellCoord coord{0, 0, 0, 0};
    cy::world::CellBuilder builder(cy::world::test::allocator(), grid.id_of(coord), coord);
    const cy::ecs::ComponentTypeId both[] = {ids->placement, ids->prop};
    const cy::ecs::ComponentTypeId one[] = {ids->placement};
    cy::world::test::Placement placement;
    cy::world::test::Prop prop;
    const void* pair[] = {&placement, &prop};
    const cy::u32 pair_sizes[] = {static_cast<cy::u32>(sizeof(placement)),
                                  static_cast<cy::u32>(sizeof(prop))};
    const void* single[] = {&placement};
    const cy::u32 single_sizes[] = {static_cast<cy::u32>(sizeof(placement))};
    for (cy::u32 row = 0; row < 400; ++row) {
        CY_REQUIRE(builder
                       .add_entity(cy::world::PersistentId{row + 1}, cy::world::kDefaultLayer, both,
                                   pair, pair_sizes)
                       .has_value());
    }
    for (cy::u32 row = 0; row < 400; ++row) {
        CY_REQUIRE(builder
                       .add_entity(cy::world::PersistentId{row + 1000}, cy::world::kDefaultLayer,
                                   one, single, single_sizes)
                       .has_value());
    }
    const cy::world::CookedCell cell = builder.finish();
    CY_REQUIRE_EQ(cell.blocks.size(), 2u);

    cy::world::CellActivation activation(cy::world::test::allocator(), cell);
    // A budget of one nanosecond does at most one step per call, so several calls are needed.
    cy::u32 calls = 0;
    while (!activation.ready() && calls < 100) {
        const auto phase = activation.advance(cell, layers, nullptr, 1);
        CY_REQUIRE(phase.has_value());
        ++calls;
        // "WHEN a system queries during a cell's preparation, THEN it SHALL observe the cell as
        // inactive, and SHALL NOT see some of its entities."
        CY_CHECK_EQ(ecs.entity_count(), 0u);
    }
    CY_CHECK(activation.ready());
    CY_CHECK_GT(calls, 1u);

    CY_REQUIRE(activation.publish(ecs, layers).has_value());
    CY_CHECK_EQ(ecs.entity_count(), 800u);
    CY_CHECK_EQ(ecs.archetypes().size(), 2u);
}

CY_TEST_CASE("publication is refused before the cell is ready, so it is never partial") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::LayerTable layers(cy::world::test::allocator());
    const cy::world::CookedCell cell = cy::world::test::cook_props(
        cy::world::test::allocator(), grid, cy::world::CellCoord{0, 0, 0, 0}, 64, *ids, 1);

    cy::world::CellActivation activation(cy::world::test::allocator(), cell);
    const cy::Status early = activation.publish(ecs, layers);
    CY_CHECK_FALSE(early.has_value());
    CY_CHECK(early.error().code == cy::ErrorCode::Unavailable);
    CY_CHECK_EQ(ecs.entity_count(), 0u);
}

CY_TEST_CASE("a layer switch publishes and withdraws whole blocks, not entities") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::LayerTable layers(cy::world::test::allocator());
    const cy::world::LayerId intact{200};
    const cy::world::LayerId destroyed{201};
    CY_REQUIRE(layers.declare(intact, cy::world::LayerKind::Runtime, "city").has_value());
    CY_REQUIRE(layers.declare(destroyed, cy::world::LayerKind::Scenario, "ruins").has_value());

    const cy::world::CellCoord coord{0, 0, 0, 0};
    cy::world::CellBuilder builder(cy::world::test::allocator(), grid.id_of(coord), coord);
    const cy::ecs::ComponentTypeId components[] = {ids->placement};
    cy::world::test::Placement placement;
    const void* values[] = {&placement};
    const cy::u32 sizes[] = {static_cast<cy::u32>(sizeof(placement))};
    for (cy::u32 row = 0; row < 20000; ++row) {
        const cy::world::LayerId layer = (row < 10000) ? intact : destroyed;
        CY_REQUIRE(
            builder.add_entity(cy::world::PersistentId{row + 1}, layer, components, values, sizes)
                .has_value());
    }
    const cy::world::CookedCell cell = builder.finish();

    cy::world::CellActivation activation(cy::world::test::allocator(), cell);
    CY_REQUIRE(activation.advance(cell, layers, nullptr, kUnbounded).has_value());
    CY_REQUIRE(activation.publish(ecs, layers).has_value());

    // Only the runtime layer is on, so only its block is in the world.
    CY_CHECK_EQ(ecs.entity_count(), 10000u);

    // "WHEN a mission destroys a city, THEN the destroyed-city layer SHALL be activated and the
    // intact layer deactivated as ONE OPERATION, not twenty thousand property changes."
    CY_REQUIRE(layers.set_state(destroyed, cy::world::LayerState::Activated).has_value());
    CY_REQUIRE(activation.publish_layer(ecs, destroyed).has_value());
    CY_REQUIRE(layers.set_state(intact, cy::world::LayerState::Unloaded).has_value());
    CY_REQUIRE(activation.withdraw_layer(ecs, intact).has_value());

    CY_CHECK_EQ(ecs.entity_count(), 10000u);
    CY_CHECK_EQ(activation.published_rows(), 10000u);
}

CY_TEST_CASE("the overlay is applied during activation, so a destroyed building never exists") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::LayerTable layers(cy::world::test::allocator());
    const cy::world::CellCoord coord{0, 0, 0, 0};
    const cy::world::CookedCell cell =
        cy::world::test::cook_props(cy::world::test::allocator(), grid, coord, 10, *ids, 1);

    cy::world::PersistenceOverlay overlay(cy::world::test::allocator());
    // Rows 0 and 9 are destroyed; row 4's placement is overridden.
    CY_REQUIRE(overlay.record_removed(cell.id, cy::world::PersistentId{1}).has_value());
    CY_REQUIRE(overlay.record_removed(cell.id, cy::world::PersistentId{10}).has_value());
    cy::world::test::Placement moved;
    moved.x = -77.0f;
    const auto* bytes = reinterpret_cast<const cy::u8*>(&moved);
    CY_REQUIRE(overlay
                   .record_component(cell.id, cy::world::PersistentId{5}, ids->placement,
                                     cy::Span<const cy::u8>(bytes, sizeof(moved)))
                   .has_value());

    cy::world::CellActivation activation(cy::world::test::allocator(), cell);
    CY_REQUIRE(activation.advance(cell, layers, &overlay, kUnbounded).has_value());
    CY_REQUIRE(activation.publish(ecs, layers).has_value());

    // Eight entities, and the two removed ones NEVER EXISTED — the authored cell was not
    // instantiated and then corrected.
    CY_CHECK_EQ(ecs.entity_count(), 8u);
    const cy::Span<const cy::ecs::Entity> entities = activation.entities();
    CY_REQUIRE_EQ(entities.size(), 8u);

    bool found_override = false;
    bool found_removed = false;
    for (const cy::ecs::Entity entity : entities) {
        const auto* placement = ecs.get<cy::world::test::Placement>(entity, ids->placement);
        CY_REQUIRE(placement != nullptr);
        found_override = found_override || placement->x == -77.0f;
        found_removed = found_removed || placement->x == 0.0f || placement->x == 9.0f;
    }
    CY_CHECK(found_override);
    CY_CHECK_FALSE(found_removed);
    // The cooked cell is untouched: authored cells are immutable.
    CY_CHECK_EQ(cell.blocks[0].count, 10u);
}

CY_TEST_CASE("runtime-created entities are staged by the same path as authored ones") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::LayerTable layers(cy::world::test::allocator());
    const cy::world::CookedCell cell = cy::world::test::cook_props(
        cy::world::test::allocator(), grid, cy::world::CellCoord{0, 0, 0, 0}, 4, *ids, 1);

    cy::world::PersistenceOverlay overlay(cy::world::test::allocator());
    cy::world::test::Placement placement;
    placement.x = 1234.0f;
    const cy::ecs::ComponentTypeId components[] = {ids->placement};
    const void* values[] = {&placement};
    const cy::u32 sizes[] = {static_cast<cy::u32>(sizeof(placement))};
    CY_REQUIRE(overlay
                   .record_created(cell.id, cy::world::PersistentId{9999}, cy::world::kDefaultLayer,
                                   components, values, sizes)
                   .has_value());

    cy::world::CellActivation activation(cy::world::test::allocator(), cell);
    CY_REQUIRE(activation.advance(cell, layers, &overlay, kUnbounded).has_value());
    CY_REQUIRE(activation.publish(ecs, layers).has_value());
    CY_CHECK_EQ(ecs.entity_count(), 5u);
}

CY_TEST_CASE("withdrawal is atomic and keeps the staging, so republishing is one instantiate") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::LayerTable layers(cy::world::test::allocator());
    const cy::world::CookedCell cell = cy::world::test::cook_props(
        cy::world::test::allocator(), grid, cy::world::CellCoord{0, 0, 0, 0}, 128, *ids, 1);

    cy::world::CellActivation activation(cy::world::test::allocator(), cell);
    CY_REQUIRE(activation.advance(cell, layers, nullptr, kUnbounded).has_value());
    CY_REQUIRE(activation.publish(ecs, layers).has_value());
    CY_CHECK_EQ(ecs.entity_count(), 128u);
    const cy::u64 staged = activation.staged_bytes();
    CY_CHECK_GT(staged, 0u);

    CY_REQUIRE(activation.withdraw(ecs).has_value());
    CY_CHECK_EQ(ecs.entity_count(), 0u);
    CY_CHECK(activation.ready());
    CY_CHECK_EQ(activation.staged_bytes(), staged);

    // Republished without being re-prepared, which is what makes turning round cheap.
    CY_REQUIRE(activation.publish(ecs, layers).has_value());
    CY_CHECK_EQ(ecs.entity_count(), 128u);

    // Release is the incremental half, after the atomic withdrawal.
    CY_REQUIRE(activation.withdraw(ecs).has_value());
    activation.release();
    CY_CHECK_EQ(activation.staged_bytes(), 0u);
    CY_CHECK(activation.phase() == cy::world::StagingPhase::Idle);
}

CY_TEST_CASE("a cell torn down mid-preparation frees its staging and publishes nothing") {
    // RULE 4 of this milestone, and the shape M5.5's gate found a real defect in: teardown UNDER
    // LOAD rather than at rest. A cell is destroyed at every phase of preparation, and at each one
    // the ECS world must be untouched afterwards.
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    cy::world::LayerTable layers(cy::world::test::allocator());
    const cy::world::CookedCell cell = cy::world::test::cook_props(
        cy::world::test::allocator(), grid, cy::world::CellCoord{0, 0, 0, 0}, 512, *ids, 1);

    for (cy::u32 steps = 0; steps < 6; ++steps) {
        cy::world::CellActivation activation(cy::world::test::allocator(), cell);
        for (cy::u32 step = 0; step < steps; ++step) {
            CY_REQUIRE(activation.advance(cell, layers, nullptr, 1).has_value());
        }
        // Destroyed right here, at whatever phase it reached.
        CY_CHECK_EQ(ecs.entity_count(), 0u);
    }
    CY_CHECK_EQ(ecs.entity_count(), 0u);
}

CY_TEST_CASE("events are queued and consumed in a declared order, never called back") {
    cy::world::CellEventQueue queue(cy::world::test::allocator());

    const auto navigation = queue.add_consumer("navigation", 10);
    const auto audio = queue.add_consumer("audio", 20);
    const auto gameplay = queue.add_consumer("gameplay", 5);
    CY_REQUIRE(navigation.has_value());
    CY_REQUIRE(audio.has_value());
    CY_REQUIRE(gameplay.has_value());

    // "Consumers SHALL be able to declare ordering requirements between themselves for a given
    // transition." The declared order is the order, not the registration order.
    CY_REQUIRE_EQ(queue.consumers().size(), 3u);
    CY_CHECK(cy::world::test::same_text(queue.consumers()[0].name, "gameplay"));
    CY_CHECK(cy::world::test::same_text(queue.consumers()[1].name, "navigation"));
    CY_CHECK(cy::world::test::same_text(queue.consumers()[2].name, "audio"));

    cy::world::CellEvent event;
    event.kind = cy::world::CellEventKind::Activated;
    event.cell = cy::world::CellId{99};
    CY_REQUIRE(queue.emit(event).has_value());
    event.kind = cy::world::CellEventKind::Resident;
    CY_REQUIRE(queue.emit(event).has_value());

    cy::Array<cy::world::CellEvent> drained(cy::world::test::allocator());
    CY_REQUIRE(queue.drain(*navigation, drained).has_value());
    CY_REQUIRE_EQ(drained.size(), 2u);
    CY_CHECK(drained[0].kind == cy::world::CellEventKind::Activated);
    CY_CHECK_LT(drained[0].sequence, drained[1].sequence);

    // Draining twice reports nothing new.
    drained.clear();
    CY_REQUIRE(queue.drain(*navigation, drained).has_value());
    CY_CHECK_EQ(drained.size(), 0u);

    // A consumer that has not drained still has its backlog, so compaction cannot drop it.
    queue.compact();
    CY_CHECK_EQ(queue.pending(), 2u);
    CY_REQUIRE(queue.drain(*audio, drained).has_value());
    CY_CHECK_EQ(drained.size(), 2u);
    drained.clear();
    CY_REQUIRE(queue.drain(*gameplay, drained).has_value());
    queue.compact();
    CY_CHECK_EQ(queue.pending(), 0u);

    CY_CHECK_FALSE(queue.drain(4242, drained).has_value());
}
