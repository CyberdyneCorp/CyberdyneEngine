// The persistence overlay and the dynamic index. Task 3.7.

#include <cy/test/test.h>

#include <cy/world/overlay.h>
#include <cy/world/partition.h>

#include "fixtures.h"

#include <cstring>

namespace {

cy::world::PartitionConfig grid_config() noexcept {
    return cy::world::uniform_grid_config(100.0f);
}

}  // namespace

CY_TEST_CASE("an unloaded region's state is available without loading it") {
    // The property that decides the data structure: "saving a world of which most is unloaded
    // requires no additional streaming". Nothing in this test constructs a `WorldStreaming`, an
    // `ecs::World` or a cell — and the overlay still answers for every cell it holds state for.
    cy::world::PersistenceOverlay overlay(cy::world::test::allocator());
    cy::world::HierarchicalGrid grid(grid_config());

    const cy::world::CellId far_away = grid.id_of(cy::world::CellCoord{200, 0, 300, 0});
    const cy::world::CellId nearby = grid.id_of(cy::world::CellCoord{0, 0, 0, 0});
    const cy::world::PersistentId building{9001};

    CY_REQUIRE(overlay.record_removed(far_away, building).has_value());
    CY_REQUIRE(overlay.set_variable(1, 42).has_value());

    CY_CHECK(overlay.is_removed(far_away, building));
    CY_CHECK_FALSE(overlay.is_removed(nearby, building));
    CY_REQUIRE(overlay.find(far_away) != nullptr);
    CY_CHECK_EQ(overlay.find(far_away)->removed.size(), 1u);
    CY_REQUIRE(overlay.variable(1) != nullptr);
    CY_CHECK_EQ(overlay.variable(1)->integer, 42);

    // The save's own iteration, sorted so that one world state is one sequence of records however
    // the player reached it.
    cy::Array<cy::world::CellId> cells(cy::world::test::allocator());
    CY_REQUIRE(overlay.cells(cells).has_value());
    CY_CHECK_EQ(cells.size(), 1u);
    CY_CHECK(cells[0] == far_away);
}

CY_TEST_CASE("an override replaces the previous value for the same entity and component") {
    cy::world::PersistenceOverlay overlay(cy::world::test::allocator());
    const cy::world::CellId cell{0x4242};
    const cy::world::PersistentId door{5};

    const cy::u32 closed = 0;
    const cy::u32 open = 1;
    const auto* closed_bytes = reinterpret_cast<const cy::u8*>(&closed);
    const auto* open_bytes = reinterpret_cast<const cy::u8*>(&open);

    CY_REQUIRE(overlay.record_component(cell, door, 3, cy::Span<const cy::u8>(closed_bytes, 4))
                   .has_value());
    CY_REQUIRE(
        overlay.record_component(cell, door, 3, cy::Span<const cy::u8>(open_bytes, 4)).has_value());

    const cy::Span<const cy::u8> value = overlay.component_override(cell, door, 3);
    CY_REQUIRE_EQ(value.size(), 4u);
    cy::u32 read = 0;
    std::memcpy(&read, value.data(), sizeof(read));
    CY_CHECK_EQ(read, 1u);
    CY_REQUIRE(overlay.find(cell) != nullptr);
    CY_CHECK_EQ(overlay.find(cell)->overrides.size(), 1u);

    // A different component on the same entity is a different record.
    CY_REQUIRE(
        overlay.record_component(cell, door, 4, cy::Span<const cy::u8>(open_bytes, 4)).has_value());
    CY_CHECK_EQ(overlay.find(cell)->overrides.size(), 2u);
    CY_CHECK_EQ(overlay.component_override(cell, door, 9).size(), 0u);
}

CY_TEST_CASE("a save against different content is detected, not silently applied") {
    cy::world::PersistenceOverlay overlay(cy::world::test::allocator());
    overlay.set_content_version(7);
    CY_CHECK(overlay.check_content_version(7).has_value());

    const cy::Status mismatch = overlay.check_content_version(8);
    CY_CHECK_FALSE(mismatch.has_value());
    CY_CHECK(mismatch.error().code == cy::ErrorCode::Unsupported);
}

CY_TEST_CASE("a vehicle crosses a hundred cells without rewriting its home cell") {
    const cy::world::PartitionConfig config = grid_config();
    cy::world::HierarchicalGrid grid(config);
    cy::world::DynamicIndex index(cy::world::test::allocator());
    cy::world::PersistenceOverlay overlay(cy::world::test::allocator());

    const cy::world::PersistentId vehicle{321};
    const cy::world::WorldPosition start =
        cy::world::from_absolute(config, cy::world::WorldVec3d{50.0, 0.0, 50.0}, 0);
    const cy::world::CellId home = grid.id_of(start.cell);
    CY_REQUIRE(index.track(vehicle, home, start).has_value());

    for (cy::u32 step = 1; step <= 100; ++step) {
        const cy::world::WorldPosition moved = cy::world::from_absolute(
            config, cy::world::WorldVec3d{50.0 + (static_cast<cy::f64>(step) * 100.0), 0.0, 50.0},
            0);
        CY_REQUIRE(index.moved(vehicle, moved).has_value());
    }

    const cy::world::DynamicIndex::Tracked* tracked = index.find(vehicle);
    CY_REQUIRE(tracked != nullptr);
    CY_CHECK_EQ(tracked->crossings, 100u);
    // The home cell — where it is PERSISTED — is untouched. That is the requirement, and the
    // assignment that would break it is the one deliberately absent from `moved()`.
    CY_CHECK(tracked->home == home);
    CY_CHECK_NE(grid.id_of(tracked->runtime_cell).value, home.value);
    // And nothing was written to the overlay by any of it.
    CY_CHECK_EQ(overlay.cell_count(), 0u);

    // A CHECKPOINT is what makes the persistent position current, and it files the record under the
    // HOME cell rather than wherever the vehicle wandered.
    CY_REQUIRE(index.checkpoint(config, overlay).has_value());
    CY_REQUIRE(overlay.find(home) != nullptr);
    CY_REQUIRE_EQ(overlay.find(home)->positions.size(), 1u);
    CY_CHECK(overlay.find(home)->positions[0].entity == vehicle);
    CY_CHECK_EQ(overlay.find(home)->positions[0].position.cell.x, 100);
}

CY_TEST_CASE("occupants of a runtime cell are found so a deactivation can apply their policy") {
    const cy::world::PartitionConfig config = grid_config();
    cy::world::HierarchicalGrid grid(config);
    cy::world::DynamicIndex index(cy::world::test::allocator());

    const cy::world::WorldPosition here =
        cy::world::from_absolute(config, cy::world::WorldVec3d{50.0, 0.0, 50.0}, 0);
    const cy::world::WorldPosition elsewhere =
        cy::world::from_absolute(config, cy::world::WorldVec3d{950.0, 0.0, 50.0}, 0);

    CY_REQUIRE(index
                   .track(cy::world::PersistentId{1}, grid.id_of(here.cell), here,
                          cy::world::MigrationPolicy::Migrate)
                   .has_value());
    CY_REQUIRE(index
                   .track(cy::world::PersistentId{2}, grid.id_of(here.cell), elsewhere,
                          cy::world::MigrationPolicy::PersistAndRemove)
                   .has_value());

    cy::Array<cy::world::PersistentId> occupants(cy::world::test::allocator());
    CY_REQUIRE(index.occupants_of(config, grid.id_of(here.cell), occupants).has_value());
    CY_REQUIRE_EQ(occupants.size(), 1u);
    CY_CHECK_EQ(occupants[0].value, 1u);

    // The declared policy is carried, so "the entity SHALL NOT be silently destroyed" has something
    // to consult.
    CY_REQUIRE(index.find(cy::world::PersistentId{2}) != nullptr);
    CY_CHECK(index.find(cy::world::PersistentId{2})->policy ==
             cy::world::MigrationPolicy::PersistAndRemove);

    CY_REQUIRE(index.forget(cy::world::PersistentId{1}).has_value());
    CY_CHECK(index.find(cy::world::PersistentId{1}) == nullptr);
    CY_CHECK_FALSE(index.forget(cy::world::PersistentId{1}).has_value());
}
