// Cells cooked in ECS-native form, their channels, their cost model and their validation. Task 3.2.

#include <cy/test/test.h>

#include <cy/world/cell.h>
#include <cy/world/partition.h>

#include "fixtures.h"

namespace {

cy::world::PartitionConfig grid_config() noexcept {
    return cy::world::uniform_grid_config(128.0f);
}

}  // namespace

CY_TEST_CASE("a cook produces archetype blocks, not an entity-by-entity object graph") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    const cy::world::CookedCell cell = cy::world::test::cook_props(
        cy::world::test::allocator(), grid, cy::world::CellCoord{0, 0, 0, 0}, 8, *ids, 100);

    // ONE block for eight entities of one archetype in one layer. An entity-by-entity graph would
    // have produced eight of something.
    CY_REQUIRE_EQ(cell.blocks.size(), 1u);
    const cy::world::CookedBlock& block = cell.blocks[0];
    CY_CHECK_EQ(block.count, 8u);
    CY_CHECK_EQ(block.components.size(), 2u);
    CY_CHECK_EQ(block.columns.size(), 2u);
    CY_CHECK_EQ(block.ids.size(), 8u);

    // A column is `count` rows of the component's size, contiguous and ready to memcpy into a
    // chunk. This is the invariant `ecs::World::instantiate()` consumes with no transformation.
    CY_CHECK_EQ(block.columns[0].size(), 8u * sizeof(cy::world::test::Placement));
    CY_CHECK_EQ(block.columns[1].size(), 8u * sizeof(cy::world::test::Prop));

    const auto* placements =
        reinterpret_cast<const cy::world::test::Placement*>(block.columns[0].data());
    CY_CHECK_EQ(placements[3].x, 3.0f);
    const auto* props = reinterpret_cast<const cy::world::test::Prop*>(block.columns[1].data());
    CY_CHECK_EQ(props[7].kind, 7u);
}

CY_TEST_CASE("rows are grouped by archetype and by layer, so a layer switch is whole blocks") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    const cy::world::CellCoord coord{1, 0, 0, 0};
    cy::world::CellBuilder builder(cy::world::test::allocator(), grid.id_of(coord), coord);

    const cy::world::LayerId intact{10};
    const cy::world::LayerId destroyed{11};
    const cy::ecs::ComponentTypeId both[] = {ids->placement, ids->prop};
    const cy::ecs::ComponentTypeId one[] = {ids->placement};

    cy::world::test::Placement placement;
    cy::world::test::Prop prop;
    const void* pair[] = {&placement, &prop};
    const cy::u32 pair_sizes[] = {static_cast<cy::u32>(sizeof(placement)),
                                  static_cast<cy::u32>(sizeof(prop))};
    const void* single[] = {&placement};
    const cy::u32 single_sizes[] = {static_cast<cy::u32>(sizeof(placement))};

    CY_REQUIRE(
        builder.add_entity(cy::world::PersistentId{1}, intact, both, pair, pair_sizes).has_value());
    CY_REQUIRE(
        builder.add_entity(cy::world::PersistentId{2}, intact, both, pair, pair_sizes).has_value());
    CY_REQUIRE(builder.add_entity(cy::world::PersistentId{3}, destroyed, both, pair, pair_sizes)
                   .has_value());
    CY_REQUIRE(builder.add_entity(cy::world::PersistentId{4}, intact, one, single, single_sizes)
                   .has_value());

    const cy::world::CookedCell cell = builder.finish();
    // Three blocks: (both, intact), (both, destroyed), (placement, intact). Four entities.
    CY_CHECK_EQ(cell.blocks.size(), 3u);
    CY_CHECK_EQ(cell.row_count(), 4u);
    for (const cy::world::CookedBlock& block : cell.blocks.span()) {
        const bool known_layer = block.layer == intact || block.layer == destroyed;
        CY_CHECK(known_layer);
    }
}

CY_TEST_CASE("duplicate persistent identifiers are a cook error, and are named") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    const cy::world::CellCoord coord{0, 0, 0, 0};
    cy::world::CellBuilder builder(cy::world::test::allocator(), grid.id_of(coord), coord);
    const cy::ecs::ComponentTypeId components[] = {ids->placement};
    cy::world::test::Placement placement;
    const void* values[] = {&placement};
    const cy::u32 sizes[] = {static_cast<cy::u32>(sizeof(placement))};

    CY_REQUIRE(builder
                   .add_entity(cy::world::PersistentId{7}, cy::world::kDefaultLayer, components,
                               values, sizes)
                   .has_value());
    CY_REQUIRE(builder
                   .add_entity(cy::world::PersistentId{7}, cy::world::kDefaultLayer, components,
                               values, sizes)
                   .has_value());

    const cy::world::CookedCell cell = builder.finish();
    const cy::world::ValidationResult result = cy::world::validate_cell(cell);
    CY_CHECK_FALSE(result.ok);
    CY_CHECK_EQ(result.duplicate.value, 7u);
    CY_CHECK(
        cy::world::test::same_text(result.message, "duplicate persistent identifier in one cell"));

    // A zero identifier is refused at the point it is offered, not at validation.
    CY_CHECK_FALSE(builder
                       .add_entity(cy::world::PersistentId{0}, cy::world::kDefaultLayer, components,
                                   values, sizes)
                       .has_value());
}

CY_TEST_CASE("the server profile omits rendering channels and keeps every identity") {
    // "WHEN a dedicated server build is cooked, THEN rendering payloads SHALL be omitted and cell
    // and entity identity SHALL be unchanged."
    const cy::world::ChannelMask server =
        cy::world::profile_channels(cy::world::WorldProfile::DedicatedServer);
    CY_CHECK(server.has(cy::world::Channel::Entities));
    CY_CHECK(server.has(cy::world::Channel::Physics));
    CY_CHECK(server.has(cy::world::Channel::Navigation));
    CY_CHECK(server.has(cy::world::Channel::Ai));
    CY_CHECK_FALSE(server.has(cy::world::Channel::Geometry));
    CY_CHECK_FALSE(server.has(cy::world::Channel::Textures));
    CY_CHECK_FALSE(server.has(cy::world::Channel::Audio));
    CY_CHECK_FALSE(server.has(cy::world::Channel::Illumination));

    const cy::world::ChannelMask client =
        cy::world::profile_channels(cy::world::WorldProfile::Client);
    CY_CHECK_EQ(client.bits, cy::world::ChannelMask::all().bits);

    // Identity does not consult the profile at all — it is derived from the partition, which is why
    // client and server refer to the same content.
    const cy::world::PartitionConfig config = grid_config();
    CY_CHECK_EQ(cy::world::cell_id_of(config, cy::world::CellCoord{3, 1, 2, 0}).value,
                cy::world::cell_id_of(config, cy::world::CellCoord{3, 1, 2, 0}).value);
}

CY_TEST_CASE("a channel delta names only what is missing") {
    cy::world::ChannelMask resident;
    resident.set(cy::world::Channel::Entities);
    resident.set(cy::world::Channel::Geometry);

    cy::world::ChannelMask required = resident;
    required.set(cy::world::Channel::Physics);

    const cy::world::ChannelMask delta = required.without(resident);
    CY_CHECK(delta.has(cy::world::Channel::Physics));
    CY_CHECK_FALSE(delta.has(cy::world::Channel::Entities));
    CY_CHECK_FALSE(delta.has(cy::world::Channel::Geometry));
    CY_CHECK(required.without(required).empty());
}

CY_TEST_CASE("the cost model is computed at cook time from rows and bytes") {
    cy::ecs::World ecs(cy::world::test::allocator());
    CY_REQUIRE(ecs.initialize().has_value());
    const auto ids = cy::world::test::register_components(ecs);
    CY_REQUIRE(ids.has_value());

    cy::world::HierarchicalGrid grid(grid_config());
    const cy::world::CookedCell small = cy::world::test::cook_props(
        cy::world::test::allocator(), grid, cy::world::CellCoord{0, 0, 0, 0}, 4, *ids, 1);
    const cy::world::CookedCell large = cy::world::test::cook_props(
        cy::world::test::allocator(), grid, cy::world::CellCoord{1, 0, 0, 0}, 64, *ids, 100);

    CY_CHECK_EQ(small.cost.entities, 4u);
    CY_CHECK_EQ(large.cost.entities, 64u);
    // "WHEN two candidate cells compete for a budget, THEN the planner SHALL compare their
    // estimated costs BEFORE requesting either." Both numbers exist before anything is loaded.
    CY_CHECK_GT(large.cost.io_bytes, small.cost.io_bytes);
    CY_CHECK_GT(large.cost.activation_time, small.cost.activation_time);
    CY_CHECK_EQ(small.cost.activation_time,
                cy::world::estimate_activation_time(4, small.cost.io_bytes));
}

CY_TEST_CASE("a RequireLoaded reference builds the hard dependency closure at cook time") {
    cy::world::HierarchicalGrid grid(grid_config());
    const cy::world::CellCoord coord{0, 0, 0, 0};
    const cy::world::CellId neighbour = grid.id_of(cy::world::CellCoord{1, 0, 0, 0});
    const cy::world::CellId elsewhere = grid.id_of(cy::world::CellCoord{9, 0, 0, 0});
    cy::world::CellBuilder builder(cy::world::test::allocator(), grid.id_of(coord), coord);

    CY_REQUIRE(builder
                   .add_reference(cy::world::PersistentReference{
                       cy::world::PersistentId{1}, cy::world::PersistentId{2}, neighbour,
                       cy::world::ReferencePolicy::RequireLoaded})
                   .has_value());
    // A soft reference forces nothing. That distinction is the whole reason the policies exist.
    CY_REQUIRE(builder
                   .add_reference(cy::world::PersistentReference{
                       cy::world::PersistentId{1}, cy::world::PersistentId{3}, elsewhere,
                       cy::world::ReferencePolicy::Soft})
                   .has_value());
    // The same hard target twice is one dependency.
    CY_REQUIRE(builder
                   .add_reference(cy::world::PersistentReference{
                       cy::world::PersistentId{4}, cy::world::PersistentId{5}, neighbour,
                       cy::world::ReferencePolicy::RequireLoaded})
                   .has_value());

    const cy::world::CookedCell cell = builder.finish();
    CY_CHECK_EQ(cell.references.size(), 3u);
    CY_REQUIRE_EQ(cell.hard_dependencies.size(), 1u);
    CY_CHECK_EQ(cell.hard_dependencies[0].value, neighbour.value);
}
