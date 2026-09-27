// SPDX-License-Identifier: MIT
// Marking a unit selected from a module, through the C ABI, as a Swift game does.
// `unit.abi_selection`.
//
// `bindings/swift/Sources/CyberdyneKit/Selection.swift` finds the engine's selection component by
// NAME and adds it from the bytes of a two-word struct. This is that path with the Swift left out:
// every call below goes through `cy_get_interface`, and nothing names the C++ component type except
// to register it in the world and to read back what the renderer would gather. It lives here rather
// than beside the component because the claim is about the boundary, which a test in
// src/rendering/ cannot include.

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/rendering/selection/selection_component.h>
#include <cy/test/test.h>

#include <cstring>

using namespace cy;
using namespace cy::rendering::selection;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

constexpr HighlightColour kTeamBlue{40, 140, 255, 255};
constexpr HighlightColour kHoverWhite{255, 244, 214, 255};

}  // namespace

CY_TEST_CASE("a module marks and unmarks a unit through the C ABI, as Swift does") {
    ecs::World world(allocator());
    CY_REQUIRE(world.initialize().has_value());
    const Expected<ecs::ComponentTypeId, Error> id = register_selection_highlight(world);
    CY_REQUIRE(id.has_value());
    abi::World binding(allocator(), world);
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    CyWorld handle = &binding;

    // Found by the name Swift's `SelectionHighlight.componentName` spells.
    const CyComponentTypeId component =
        iface->world_find_component(handle, "cy::rendering::selection::SelectionHighlight");
    CY_REQUIRE(component != CY_COMPONENT_TYPE_INVALID);
    CY_CHECK_EQ(static_cast<ecs::ComponentTypeId>(component), *id);

    // Added from the bytes of a two-word struct — `kind`, then `colour` — which is Swift's layout.
    const CyEntity unit = iface->world_create_entity(handle);
    CY_REQUIRE(unit != CY_ENTITY_NULL);
    const u32 bytes[2] = {1U, kTeamBlue.packed()};
    CY_REQUIRE(iface->world_add_component(handle, unit, component, bytes) == CY_RESULT_OK);

    HighlightSet set(allocator());
    CY_REQUIRE(gather_highlights(world, *id, set).has_value());
    const HighlightMark* mark = set.find(unit);
    CY_REQUIRE(mark != nullptr);
    CY_CHECK(mark->kind == HighlightKind::Selected);
    CY_CHECK(mark->colour == kTeamBlue);

    // Changed in place through a borrow — the cursor moved onto the unit.
    const CyBorrow borrow = iface->world_borrow_component(handle, unit, component);
    CY_REQUIRE(borrow.data != nullptr);
    const u32 hovered[2] = {2U, kHoverWhite.packed()};
    std::memcpy(borrow.data, hovered, sizeof(hovered));
    CY_REQUIRE(gather_highlights(world, *id, set).has_value());
    CY_REQUIRE(set.find(unit) != nullptr);
    CY_CHECK(set.find(unit)->kind == HighlightKind::Hovered);
    CY_CHECK(set.find(unit)->colour == kHoverWhite);

    // And removed: the selection was cleared.
    CY_REQUIRE(iface->world_remove_component(handle, unit, component) == CY_RESULT_OK);
    CY_REQUIRE(gather_highlights(world, *id, set).has_value());
    CY_CHECK(set.empty());
}
