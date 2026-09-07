// Coordinates, cell identity, and the partitioner. Task 3.1.

#include <cy/test/test.h>

#include <cy/world/partition.h>

#include "fixtures.h"

namespace {

cy::world::PartitionConfig three_levels() noexcept {
    cy::world::PartitionConfig config;
    config.partition = 0;
    config.base_cell_size = 128.0f;
    config.levels = 3;
    config.level_ratio = 4;
    return config;
}

}  // namespace

CY_TEST_CASE("a cell identifier is stable across runs and opaque to its coordinate") {
    const cy::world::PartitionConfig config = three_levels();
    const cy::world::CellCoord coord{12, -3, 7, 1};

    const cy::world::CellId first = cy::world::cell_id_of(config, coord);
    const cy::world::CellId second = cy::world::cell_id_of(config, coord);
    CY_CHECK_EQ(first.value, second.value);
    CY_CHECK(first.is_valid());

    // Opaque: nothing about the coordinate is readable out of it. The weakest honest check is that
    // it is not the coordinate packed into bits, which is what a consumer would start doing
    // arithmetic on.
    const auto packed = static_cast<cy::u64>(static_cast<cy::u32>(coord.x));
    CY_CHECK_NE(first.value, packed);

    // Neighbours differ. A hash that collided here would make two cells one.
    CY_CHECK_NE(cy::world::cell_id_of(config, cy::world::CellCoord{13, -3, 7, 1}).value,
                first.value);
    CY_CHECK_NE(cy::world::cell_id_of(config, cy::world::CellCoord{12, -3, 7, 0}).value,
                first.value);
}

CY_TEST_CASE("content may change; partition settings changing is what invalidates cell identity") {
    const cy::world::PartitionConfig before = three_levels();
    const cy::world::CellCoord coord{4, 0, -9, 0};
    const cy::world::CellId original = cy::world::cell_id_of(before, coord);

    // "WHEN content changes but partition settings do not, THEN cell identifiers SHALL be unchanged
    // and existing saves SHALL remain valid." Content is not an input to the identifier at all,
    // which is what makes that true rather than merely likely.
    cy::world::PartitionConfig same = three_levels();
    CY_CHECK_EQ(cy::world::cell_id_of(same, coord).value, original.value);
    const cy::world::PartitionChange unchanged = cy::world::compare_partitions(before, same);
    CY_CHECK_FALSE(unchanged.identity_changed);

    // "WHEN cell size is changed, THEN the build SHALL report that cell identity changes and what
    // depends on it."
    cy::world::PartitionConfig resized = three_levels();
    resized.base_cell_size = 256.0f;
    CY_CHECK_NE(cy::world::cell_id_of(resized, coord).value, original.value);
    const cy::world::PartitionChange change = cy::world::compare_partitions(before, resized);
    CY_CHECK(change.identity_changed);
    CY_CHECK(cy::world::test::same_text(
        change.reason,
        "cell size changed: every cell identifier changes, and saves, patches, "
        "streaming caches and build caches all key on them"));

    cy::world::PartitionConfig moved = three_levels();
    moved.origin.x = 1.0;
    const cy::world::PartitionChange origin_change = cy::world::compare_partitions(before, moved);
    CY_CHECK(origin_change.identity_changed);
    CY_CHECK_NE(origin_change.previous_signature, origin_change.current_signature);
}

CY_TEST_CASE("a position is exact in 32-bit local coordinates a thousand kilometres out") {
    const cy::world::PartitionConfig config = three_levels();

    // "WHEN an entity is 1 000 km from the world origin, THEN its position SHALL be exact in 32-bit
    // local coordinates within its cell, without rebasing logic in gameplay code."
    const cy::world::WorldVec3d far_away{1'000'000.03125, -1'000'000.03125, 250'000.125};
    const cy::world::WorldPosition position = cy::world::from_absolute(config, far_away, 0);
    const cy::world::WorldVec3d round_trip = cy::world::to_absolute(config, position);

    CY_CHECK_EQ(round_trip.x, far_away.x);
    CY_CHECK_EQ(round_trip.y, far_away.y);
    CY_CHECK_EQ(round_trip.z, far_away.z);

    // The local offset stays inside the cell, which is the whole reason the precision holds.
    CY_CHECK_GE(position.local.x, 0.0f);
    CY_CHECK_LT(position.local.x, config.base_cell_size);
    CY_CHECK_GE(position.local.y, 0.0f);
    CY_CHECK_LT(position.local.y, config.base_cell_size);

    // The naive f32 form of the same coordinate cannot represent it: this is the comparison the
    // precision policy rests on, done rather than asserted.
    CY_CHECK_NE(static_cast<cy::f64>(static_cast<cy::f32>(far_away.x)), far_away.x);
}

CY_TEST_CASE("simulation-local coordinates are exact relative to a region origin") {
    const cy::world::PartitionConfig config = three_levels();
    const cy::world::WorldPosition position =
        cy::world::from_absolute(config, cy::world::WorldVec3d{5000.25, 300.5, -700.75}, 0);
    const cy::world::CellCoord region = position.cell;

    const cy::Vec3 local = cy::world::to_simulation_local(config, position, region);
    CY_CHECK_EQ(local.x, position.local.x);
    CY_CHECK_EQ(local.y, position.local.y);
    CY_CHECK_EQ(local.z, position.local.z);

    // A neighbour's origin shifts by exactly one cell.
    const cy::world::CellCoord neighbour{region.x - 1, region.y, region.z, region.level};
    const cy::Vec3 shifted = cy::world::to_simulation_local(config, position, neighbour);
    CY_CHECK_NEAR(shifted.x - local.x, config.base_cell_size, 0.01f);
}

CY_TEST_CASE("normalizing rebases a local offset past a boundary into the next cell") {
    const cy::world::PartitionConfig config = three_levels();
    cy::world::WorldPosition drifted;
    drifted.cell = cy::world::CellCoord{2, 0, 0, 0};
    drifted.local = cy::Vec3{300.0f, -10.0f, 0.0f};

    const cy::world::WorldPosition rebased = cy::world::normalized(config, drifted);
    CY_CHECK_EQ(rebased.cell.x, 4);
    CY_CHECK_EQ(rebased.cell.y, -1);
    CY_CHECK_NEAR(rebased.local.x, 44.0f, 0.01f);
    CY_CHECK_NEAR(rebased.local.y, 118.0f, 0.01f);

    // Same point in space, both ways.
    const cy::world::WorldVec3d before = cy::world::to_absolute(config, drifted);
    const cy::world::WorldVec3d after = cy::world::to_absolute(config, rebased);
    CY_CHECK_NEAR(after.x - before.x, 0.0, 0.001);
    CY_CHECK_NEAR(after.y - before.y, 0.0, 0.001);
}

CY_TEST_CASE("bounds decide the level and the cell, not the pivot") {
    cy::world::HierarchicalGrid grid(three_levels());

    // "WHEN an entity's pivot lies in one cell but its bounds span several, THEN assignment SHALL
    // account for its bounds." The wall's pivot is at the far left of a 400 m span; assignment must
    // not put it in the pivot's level-0 cell.
    cy::world::EntityPlacement wall;
    wall.id = cy::world::PersistentId{1};
    wall.bounds = cy::Aabb::from_min_max(cy::Vec3{0.0f, 0.0f, 0.0f}, cy::Vec3{400.0f, 8.0f, 2.0f});

    const cy::world::Assignment assigned = grid.assign(wall);
    CY_CHECK(assigned.spatial);
    CY_CHECK_FALSE(assigned.forced_to_coarsest);
    // 400 m does not fit a 128 m cell but fits a 512 m one, which is level 1.
    CY_CHECK_EQ(assigned.coord.level, 1u);
    CY_CHECK(grid.bounds_of(assigned.coord).contains(wall.bounds.center()));

    // A small prop at the same pivot lands at level 0, which is the point of the hierarchy.
    cy::world::EntityPlacement prop;
    prop.id = cy::world::PersistentId{2};
    prop.bounds =
        cy::Aabb::from_center_extents(cy::Vec3{0.0f, 0.0f, 0.0f}, cy::Vec3{0.25f, 0.25f, 0.25f});
    CY_CHECK_EQ(grid.assign(prop).coord.level, 0u);
}

CY_TEST_CASE("a mountain is not duplicated, and oversized content is reported") {
    cy::world::HierarchicalGrid grid(three_levels());

    // A three-kilometre landform: 128 -> 512 -> 2048, so it does not fit any level and lands on the
    // coarsest with `forced_to_coarsest` set. "The cook report SHALL name it, since it will keep a
    // large region resident."
    cy::world::EntityPlacement landform;
    landform.id = cy::world::PersistentId{3};
    landform.bounds =
        cy::Aabb::from_min_max(cy::Vec3{0.0f, 0.0f, 0.0f}, cy::Vec3{3000.0f, 400.0f, 3000.0f});

    const cy::world::Assignment assigned = grid.assign(landform);
    CY_CHECK(assigned.forced_to_coarsest);
    CY_CHECK_EQ(assigned.coord.level, 2u);

    // ONE cell, not hundreds: assignment returns a single coordinate by construction, and there is
    // no path in the interface that duplicates an entity into every overlapping cell.
    CY_CHECK(assigned.cell.is_valid());
}

CY_TEST_CASE("global, owner-managed and transient content is not assigned to a spatial cell") {
    cy::world::HierarchicalGrid grid(three_levels());
    for (const cy::world::StreamingPolicy policy :
         {cy::world::StreamingPolicy::AlwaysLoaded, cy::world::StreamingPolicy::RuntimeManaged,
          cy::world::StreamingPolicy::OwnerManaged, cy::world::StreamingPolicy::Transient}) {
        cy::world::EntityPlacement rules;
        rules.id = cy::world::PersistentId{4};
        rules.policy = policy;
        rules.bounds = cy::Aabb::from_point(cy::Vec3{10.0f, 0.0f, 10.0f});
        const cy::world::Assignment assigned = grid.assign(rules);
        CY_CHECK_FALSE(assigned.spatial);
        CY_CHECK_FALSE(assigned.cell.is_valid());
    }
}

CY_TEST_CASE("a uniform grid is a hierarchy of one level, and overlap enumeration is ordered") {
    cy::world::HierarchicalGrid grid(cy::world::uniform_grid_config(100.0f));
    CY_CHECK_EQ(grid.level_count(), 1u);

    cy::Array<cy::world::CellCoord> cells(cy::world::test::allocator());
    const cy::Aabb region =
        cy::Aabb::from_min_max(cy::Vec3{10.0f, 10.0f, 10.0f}, cy::Vec3{210.0f, 10.0f, 110.0f});
    CY_REQUIRE(grid.cells_overlapping(region, 0, cells).has_value());
    CY_CHECK_EQ(cells.size(), 6u);  // 3 in x, 1 in y, 2 in z

    // Ascending z, then y, then x — a deterministic order, so two machines planning one frame
    // request the same cells in the same sequence.
    for (cy::usize index = 1; index < cells.size(); ++index) {
        const cy::world::CellCoord& previous = cells[index - 1];
        const cy::world::CellCoord& current = cells[index];
        const bool ordered =
            (current.z > previous.z) || (current.z == previous.z && current.y > previous.y) ||
            (current.z == previous.z && current.y == previous.y && current.x > previous.x);
        CY_CHECK(ordered);
    }
}
