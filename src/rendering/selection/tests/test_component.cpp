// SPDX-License-Identifier: MIT
// Marking entities from gameplay: the component and the gather. `unit.rendering_selection`.
//
// The path a Swift module takes to the same component — by name, through `cy_get_interface` — is
// `unit.abi_selection` in src/abi/tests/, because a test at this layer cannot name the ABI above
// it.

#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/rendering/selection/selection_component.h>
#include <cy/test/test.h>

using namespace cy;
using namespace cy::rendering::selection;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

constexpr HighlightColour kTeamBlue{40, 140, 255, 255};
constexpr HighlightColour kHoverWhite{255, 244, 214, 255};

}  // namespace

CY_TEST_CASE("a unit's component marks it, keyed by the entity's identity") {
    ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const Expected<ecs::ComponentTypeId, Error> id = register_selection_highlight(world);
    CY_REQUIRE(id.has_value());
    // Registered once per world, by name.
    const Expected<ecs::ComponentTypeId, Error> again = register_selection_highlight(world);
    CY_REQUIRE(again.has_value());
    CY_CHECK_EQ(*again, *id);
    CY_CHECK_NE(world.components().find(kSelectionHighlightComponentName), nullptr);

    const Expected<ecs::Entity, Error> a = world.create();
    const Expected<ecs::Entity, Error> b = world.create();
    const Expected<ecs::Entity, Error> idle = world.create();
    const Expected<ecs::Entity, Error> bare = world.create();
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(b.has_value());
    CY_REQUIRE(idle.has_value());
    CY_REQUIRE(bare.has_value());
    const SelectionHighlight selected = SelectionHighlight::selected(kTeamBlue);
    const SelectionHighlight hovered = SelectionHighlight::hovered(kHoverWhite);
    const SelectionHighlight unmarked{};
    CY_REQUIRE(world.add(*a, *id, &selected).has_value());
    CY_REQUIRE(world.add(*b, *id, &hovered).has_value());
    CY_REQUIRE(world.add(*idle, *id, &unmarked).has_value());

    HighlightSet set(allocator());
    // A stale mark from an earlier frame is replaced, not kept.
    CY_REQUIRE(set.select(424242, kTeamBlue).has_value());
    const Expected<GatherReport, Error> gathered = gather_highlights(world, *id, set);
    CY_REQUIRE(gathered.has_value());
    CY_CHECK_EQ(gathered->marked, 2U);
    CY_CHECK_EQ(set.marks().size(), 2U);
    CY_CHECK_EQ(set.find(424242), nullptr);
    const HighlightMark* mark_a = set.find(a->bits());
    const HighlightMark* mark_b = set.find(b->bits());
    CY_REQUIRE(mark_a != nullptr);
    CY_REQUIRE(mark_b != nullptr);
    CY_CHECK(mark_a->kind == HighlightKind::Selected);
    CY_CHECK(mark_a->colour == kTeamBlue);
    CY_CHECK(mark_b->kind == HighlightKind::Hovered);
    CY_CHECK(mark_b->colour == kHoverWhite);
    CY_CHECK_EQ(set.find(idle->bits()), nullptr);
    CY_CHECK_EQ(set.find(bare->bits()), nullptr);

    // A kind this build does not know is skipped and counted, never drawn as something else.
    const SelectionHighlight unknown{7, kTeamBlue.packed()};
    CY_REQUIRE(world.add(*bare, *id, &unknown).has_value());
    const Expected<GatherReport, Error> again_gathered = gather_highlights(world, *id, set);
    CY_REQUIRE(again_gathered.has_value());
    CY_CHECK_EQ(again_gathered->skipped, 1U);
    CY_CHECK_EQ(set.find(bare->bits()), nullptr);
}
