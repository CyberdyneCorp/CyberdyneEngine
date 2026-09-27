// SPDX-License-Identifier: MIT
#pragma once
// Marking an ENTITY selected or hovered, from gameplay: a component, and the gather that turns the
// world's marks into the `HighlightSet` the outline pass draws.
//
// ================================================================================================
// WHY A COMPONENT, AND WHY IT IS REACHABLE FROM SWIFT WITHOUT A NEW ABI ENTRY
// ================================================================================================
//
// A strategy game's selection is gameplay state that the renderer shows: the player drags a box,
// the game decides which units it caught, and those units are outlined until the selection
// changes. The game already speaks to the engine through components, in C++ and in Swift alike, so
// a mark is a component on the unit's entity:
//
//     world.add(unit, highlight_id, &SelectionHighlight::selected(kTeamBlue));   // C++
//     try world.highlight(unit, .selected(r: 40, g: 140, b: 255))                // Swift
//
// and once a frame the view gathers them: `gather_highlights(world, id, highlights)`. The mark is
// keyed by `Entity::bits()` — the stable identity the extract stage gives the entity's draws — so
// the gathered set names exactly the draws the frame will issue for those entities.
//
// The component is registered BY NAME (`ComponentRegistry::register_builtin`), which is what makes
// `CyInterface::world_find_component` find it, `world_add_component` add it from the bytes of a
// matching Swift struct, and `world_borrow_component` expose them for a change. Swift's
// `CyberdyneKit.SelectionHighlight` is that struct; bindings/swift/Sources/CyberdyneKit/
// Selection.swift says so from the other side, and `unit.abi_selection` drives the whole path
// through `cy_get_interface` as a Swift module would.
//
// PRESENTATION, NOT AUTHORING. A mark is what one player's view shows this moment: it is never
// authored into a level, never saved and never replicated. So it has no manifest identifier and
// no reflected fields — the name route is the one the engine's own non-authored components take.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/ecs/world.h>
#include <cy/rendering/selection/highlight.h>

#include <type_traits>

namespace cy::rendering::selection {

/// One entity's mark. Eight bytes, two words, so a foreign struct of two 32-bit fields is it.
struct SelectionHighlight {
    /// `HighlightKind`: 1 selected, 2 hovered. Zero is "present but unmarked", which a game that
    /// toggles marks every frame can leave in place rather than removing the component.
    u32 kind = 0;
    /// `HighlightColour::packed()`: display-referred RGBA8, red in the low byte.
    u32 colour = 0;

    [[nodiscard]] static constexpr SelectionHighlight selected(HighlightColour colour) noexcept {
        return SelectionHighlight{static_cast<u32>(HighlightKind::Selected), colour.packed()};
    }
    [[nodiscard]] static constexpr SelectionHighlight hovered(HighlightColour colour) noexcept {
        return SelectionHighlight{static_cast<u32>(HighlightKind::Hovered), colour.packed()};
    }
};

static_assert(sizeof(SelectionHighlight) == 8 && alignof(SelectionHighlight) == 4);
static_assert(std::is_trivially_copyable_v<SelectionHighlight>);

/// The name the component is registered and found under. Swift's `SelectionHighlight.componentName`
/// is the same string.
inline constexpr const char* kSelectionHighlightComponentName =
    "cy::rendering::selection::SelectionHighlight";

/// Register the component in `world`, or return the id it already has there.
[[nodiscard]] Expected<ecs::ComponentTypeId, Error> register_selection_highlight(
    ecs::World& world) noexcept;

/// Replace `out`'s marks with the world's: one per entity whose `SelectionHighlight` names a kind,
/// keyed by the entity's identity. A kind this build does not know is skipped and counted.
struct GatherReport {
    u32 marked = 0;
    u32 skipped = 0;
};
[[nodiscard]] Expected<GatherReport, Error> gather_highlights(ecs::World& world,
                                                              ecs::ComponentTypeId component,
                                                              HighlightSet& out) noexcept;

}  // namespace cy::rendering::selection
