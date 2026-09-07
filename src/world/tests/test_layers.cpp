// Layers, HLOD proxies and representation tiers. Task 3.6.

#include <cy/test/test.h>

#include <cy/world/hlod.h>
#include <cy/world/layers.h>

#include "fixtures.h"

CY_TEST_CASE("a layer is identified by a number, so renaming it is safe") {
    cy::world::LayerTable layers(cy::world::test::allocator());
    const cy::world::LayerId seasonal{42};

    CY_REQUIRE(layers.declare(seasonal, cy::world::LayerKind::Variant, "winter").has_value());
    CY_REQUIRE(layers.set_state(seasonal, cy::world::LayerState::Activated).has_value());

    // "WHEN a layer is renamed, THEN entity membership SHALL be unaffected, since membership keys
    // on the identifier." Re-declaring under a new name keeps the state and the identity.
    CY_REQUIRE(layers.declare(seasonal, cy::world::LayerKind::Variant, "deep-winter").has_value());
    CY_CHECK(layers.state_of(seasonal) == cy::world::LayerState::Activated);
    CY_REQUIRE(layers.find(seasonal) != nullptr);
    CY_CHECK(cy::world::test::same_text(layers.find(seasonal)->name, "deep-winter"));

    CY_CHECK_FALSE(
        layers.declare(cy::world::LayerId{0}, cy::world::LayerKind::Runtime).has_value());
}

CY_TEST_CASE("an editor-only layer is not cooked and cannot be activated") {
    cy::world::LayerTable layers(cy::world::test::allocator());
    const cy::world::LayerId notes{7};
    CY_REQUIRE(
        layers.declare(notes, cy::world::LayerKind::EditorOnly, "designer notes").has_value());

    CY_CHECK_FALSE(layers.is_cooked(notes));
    CY_CHECK_FALSE(layers.set_state(notes, cy::world::LayerState::Activated).has_value());
    CY_CHECK(layers.state_of(notes) == cy::world::LayerState::Unloaded);

    // The default layer is cooked and activated without being declared: a world with no use for
    // layers should not have to declare one to have its entities exist.
    CY_CHECK(layers.is_cooked(cy::world::kDefaultLayer));
    CY_CHECK(layers.is_activated(cy::world::kDefaultLayer));
}

CY_TEST_CASE("a scenario layer is loaded before it is switched, and replication is one pair") {
    cy::world::LayerTable layers(cy::world::test::allocator());
    const cy::world::LayerId intact{100};
    const cy::world::LayerId destroyed{101};

    CY_REQUIRE(layers.declare(intact, cy::world::LayerKind::Runtime, "city").has_value());
    CY_REQUIRE(
        layers.declare(destroyed, cy::world::LayerKind::Scenario, "city-destroyed").has_value());
    // A runtime layer is on by default; a scenario is not, because the point of one is that
    // something switches it.
    CY_CHECK(layers.state_of(intact) == cy::world::LayerState::Activated);
    CY_CHECK(layers.state_of(destroyed) == cy::world::LayerState::Unloaded);

    // "WHEN a scenario layer is expected soon, THEN it SHALL be loaded in advance and activated
    // instantly when the event occurs."
    CY_REQUIRE(layers.set_state(destroyed, cy::world::LayerState::Loaded).has_value());
    CY_REQUIRE(layers.set_state(destroyed, cy::world::LayerState::Activated).has_value());
    CY_REQUIRE(layers.set_state(intact, cy::world::LayerState::Unloaded).has_value());

    // "WHEN the server changes a layer's state, THEN it SHALL replicate the layer identifier and
    // state, not the individual entity changes." Three changes, three pairs.
    cy::Array<cy::world::LayerTable::StateChange> changes(cy::world::test::allocator());
    CY_REQUIRE(layers.drain_changes(changes).has_value());
    CY_REQUIRE_EQ(changes.size(), 3u);
    CY_CHECK(changes[0].layer == destroyed);
    CY_CHECK(changes[0].state == cy::world::LayerState::Loaded);
    CY_CHECK(changes[2].layer == intact);
    CY_CHECK(changes[2].state == cy::world::LayerState::Unloaded);

    // Drained once. A second drain reports nothing new rather than the whole history.
    cy::Array<cy::world::LayerTable::StateChange> again(cy::world::test::allocator());
    CY_REQUIRE(layers.drain_changes(again).has_value());
    CY_CHECK_EQ(again.size(), 0u);
}

CY_TEST_CASE("a proxy is visible exactly while its cells are not, and the swap is one step") {
    cy::world::HlodRegistry hlod(cy::world::test::allocator());
    const cy::world::CellId district{0x1111};
    const cy::world::CellId block_a{0x2222};
    const cy::world::CellId block_b{0x3333};

    const cy::world::CellId covered[] = {block_a, block_b};
    CY_REQUIRE(hlod.declare(district, cy::AssetId(1, 2), 2, covered).has_value());

    // Nothing is activated: the proxy stands in. "WHEN two thousand buildings are viewed from far
    // away, THEN an aggregate proxy SHALL be rendered."
    CY_REQUIRE(hlod.refresh([](cy::world::CellId) noexcept { return false; }).has_value());
    CY_CHECK(hlod.is_visible(district));
    CY_CHECK_EQ(hlod.visible_count(), 1u);

    // One of the two activates. The proxy stays: the district is not fully there yet, and hiding it
    // now would be the visible hole the requirement exists to prevent.
    CY_REQUIRE(
        hlod.refresh([&](cy::world::CellId cell) noexcept { return cell == block_a; }).has_value());
    CY_CHECK(hlod.is_visible(district));

    // Everything published: replaced, in this same step. "The proxy SHALL be replaced in the same
    // frame the cells are published."
    const cy::u64 before = hlod.swaps();
    CY_REQUIRE(hlod.refresh([](cy::world::CellId) noexcept { return true; }).has_value());
    CY_CHECK_FALSE(hlod.is_visible(district));
    CY_CHECK_EQ(hlod.swaps(), before + 1);

    // And back, when the region unloads again.
    CY_REQUIRE(hlod.refresh([](cy::world::CellId) noexcept { return false; }).has_value());
    CY_CHECK(hlod.is_visible(district));
    CY_CHECK_EQ(hlod.swaps(), before + 2);
}

CY_TEST_CASE("promotion and demotion preserve identity and gameplay state") {
    cy::world::RepresentationTable table(cy::world::test::allocator());

    cy::world::Representation army;
    army.id = cy::world::PersistentId{500};
    army.tier = cy::world::RepresentationTier::Statistical;
    army.population = 1200;
    army.gameplay_state = 0xDEADBEEFull;
    CY_REQUIRE(table.add(army).has_value());

    // "WHEN a hostile force is far from any streaming source, THEN it SHALL exist as an aggregate
    // with position, strength and destination, and its individual units SHALL not be instantiated."
    CY_REQUIRE(table.find(army.id) != nullptr);
    CY_CHECK_EQ(table.find(army.id)->materialised, 0u);

    // "WHEN a player approaches that force, THEN individuals SHALL be materialised CONSISTENTLY
    // with the aggregate's state, not regenerated arbitrarily."
    CY_REQUIRE(table.set_tier(army.id, cy::world::RepresentationTier::Full).has_value());
    CY_CHECK_EQ(table.find(army.id)->materialised, 1200u);
    CY_CHECK_EQ(table.find(army.id)->population, 1200u);
    CY_CHECK_EQ(table.find(army.id)->gameplay_state, 0xDEADBEEFull);

    // Demoted again, and it has not silently changed.
    CY_REQUIRE(table.set_tier(army.id, cy::world::RepresentationTier::Aggregate).has_value());
    CY_CHECK_EQ(table.find(army.id)->population, 1200u);
    CY_CHECK_EQ(table.find(army.id)->gameplay_state, 0xDEADBEEFull);
    CY_CHECK_EQ(table.find(army.id)->materialised, 0u);

    CY_CHECK_FALSE(table.set_tier(cy::world::PersistentId{999}, cy::world::RepresentationTier::Full)
                       .has_value());
}
