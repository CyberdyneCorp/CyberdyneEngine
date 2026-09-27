// SPDX-License-Identifier: MIT
// Selection.swift — marking a unit selected or hovered, so the engine outlines it.
//
// The engine's half is `src/rendering/selection/`: a mask of the marked objects and an edge pass
// over the tonemapped frame. Gameplay's half is one component on the unit's entity,
// `cy::rendering::selection::SelectionHighlight`, which the engine registers BY NAME. This struct is
// that component's layout — two 32-bit words, `kind` then `colour` — and the extension below finds
// it by that name and adds it from these bytes, through the ABI entries every component goes
// through. No new ABI entry was needed, which is the point: a mark is gameplay state the renderer
// shows, not a rendering call.
//
//     try world.highlight(unit, .selected(red: 40, green: 140, blue: 255))
//     try world.highlight(hoveredUnit, .hovered(red: 255, green: 244, blue: 214))
//     try world.clearHighlight(unit)
//
// The colour is display-referred RGBA8 — the bytes the outline lands as in the output.
// `unit.abi_selection` drives this exact path from C++.

import CyberdyneABI
import CyberdyneCore

/// `cy::rendering::selection::SelectionHighlight`. Eight bytes; the engine checks the same size.
public struct SelectionHighlight: Component, Equatable, Sendable {
    /// The name the engine registers the component under, and the one `find(component:)` looks up.
    public static let componentName = "cy::rendering::selection::SelectionHighlight"
    /// The two words, as the engine lays them out. Described for a tool that lists them; the engine
    /// registers the component by name and reads the bytes.
    public static let componentFields: [FieldDescriptor] = [
        FieldDescriptor(name: "kind", type: .u32, offset: 0, size: 4),
        FieldDescriptor(name: "colour", type: .u32, offset: 4, size: 4),
    ]

    /// `HighlightKind`: 1 selected, 2 hovered, 0 present but unmarked.
    public var kind: UInt32
    /// RGBA8, red in the low byte.
    public var colour: UInt32

    /// Present but unmarked: a unit that keeps the component and draws no outline.
    public init() {
        kind = 0
        colour = 0
    }

    /// A mark of a kind and a packed colour; `selected` and `hovered` below are the usual spellings.
    public init(kind: UInt32, colour: UInt32) {
        self.kind = kind
        self.colour = colour
    }

    /// `HighlightKind::Selected`: a crisp, solid outline.
    public static let selectedKind: UInt32 = 1
    /// `HighlightKind::Hovered`: a softer glow that falls off away from the silhouette.
    public static let hoveredKind: UInt32 = 2

    /// The packed colour, red in the low byte, as the output texel reads back.
    public static func pack(red: UInt8, green: UInt8, blue: UInt8, alpha: UInt8 = 255) -> UInt32 {
        UInt32(red) | (UInt32(green) << 8) | (UInt32(blue) << 16) | (UInt32(alpha) << 24)
    }

    /// A solid outline: the unit is in the player's selection.
    public static func selected(
        red: UInt8, green: UInt8, blue: UInt8, alpha: UInt8 = 255
    )
        -> SelectionHighlight
    {
        SelectionHighlight(
            kind: selectedKind, colour: pack(red: red, green: green, blue: blue, alpha: alpha))
    }

    /// A softer glow: the cursor is over the unit.
    public static func hovered(
        red: UInt8, green: UInt8, blue: UInt8, alpha: UInt8 = 255
    )
        -> SelectionHighlight
    {
        SelectionHighlight(
            kind: hoveredKind, colour: pack(red: red, green: green, blue: blue, alpha: alpha))
    }
}

extension World {
    /// The engine's selection component in this world. Throws when the view that draws this world
    /// has not registered it — a world nothing outlines has nowhere to put a mark.
    public func selectionHighlightComponent() throws -> ComponentType {
        let component = find(component: SelectionHighlight.componentName)
        guard component.isValid else { throw CyberdyneError.fromLastError(.notFound) }
        return component
    }

    /// Mark `entity`, replacing any mark it had.
    public func highlight(_ entity: Entity, _ highlight: SelectionHighlight) throws {
        let component = try selectionHighlightComponent()
        if has(component, on: entity) {
            try remove(component, from: entity)
        }
        try add(highlight, as: component, to: entity)
    }

    /// Unmark `entity`. Unmarking an entity that has no mark is not an error.
    public func clearHighlight(_ entity: Entity) throws {
        let component = try selectionHighlightComponent()
        if has(component, on: entity) {
            try remove(component, from: entity)
        }
    }
}
