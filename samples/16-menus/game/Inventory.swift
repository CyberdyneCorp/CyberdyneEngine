// SPDX-License-Identifier: MIT
// Inventory.swift — a grid of slots, moved by dragging with the pointer or by picking up and putting
// down with Accept, with a tooltip and a details panel for what each slot holds.
//
// The slots are the document's buttons (`slot-0` to `slot-17`). Two ways to move an item:
//
//   * THE POINTER: press on a slot that holds something and move: a ghost of the item follows the
//     pointer — a 1.6 label on the screen's root, above every widget — and releasing it over
//     another slot swaps the two.
//   * A KEYBOARD OR A GAMEPAD: Accept on a slot picks its item up (the slot is outlined), the
//     d-pad moves the focus, and Accept on another slot puts it down there, swapping. Cancel puts
//     it back.
//
// Every slot carries its item's description as a tooltip — shown when the pointer rests on it — and
// the slot with focus shows its item in the details panel, which is how a gamepad player reads it.

import CyberdyneKit

struct Item {
    let name: String
    let glyph: String
    let description: String

    static let catalogue: [Item] = [
        Item(
            name: "Health potion", glyph: "Potion",
            description: "[color=#3ccf91]Restores 25 health[/color] when drunk."),
        Item(name: "Iron sword", glyph: "Sword", description: "A plain blade. 12 damage."),
        Item(name: "Oak shield", glyph: "Shield", description: "Blocks a third of a blow."),
        Item(name: "Old map", glyph: "Map", description: "Marks a cave north of the river."),
        Item(name: "Brass key", glyph: "Key", description: "Opens something brass."),
        Item(name: "Ruby", glyph: "Ruby", description: "[color=#ff5a5a]Worth 300 gold[/color]."),
        Item(name: "Bread", glyph: "Bread", description: "Restores 5 health. Slowly."),
        Item(name: "Torch", glyph: "Torch", description: "Lights a room for ten minutes."),
    ]
    static let potion = 0
}

struct Inventory {
    static let slotCount = 18
    let document: UIDocument
    /// Each slot's item, by its index in `Item.catalogue`.
    private(set) var slots: [Int?]
    /// The slot whose item a keyboard or a gamepad picked up.
    private(set) var held: Int?
    /// A pointer drag: the slot it started on, where it started, and the ghost once it moved.
    private var press: (slot: Int, at: Vec2)?
    private var ghost: UIElement?
    /// A drag ended this frame: the toolkit's click on release is not a pick.
    private var dragEnded = false
    private(set) var picks = 0
    private(set) var drops = 0
    private(set) var drags = 0

    init(document: UIDocument) {
        self.document = document
        slots = Array(repeating: nil, count: Self.slotCount)
        for (slot, item) in [(0, 0), (1, 1), (2, 2), (4, 3), (7, 4), (9, 5), (12, 6), (13, 7)] {
            slots[slot] = item
        }
    }

    func widget(_ slot: Int) -> Widget { document["slot-\(slot)"] }

    /// Every slot shows its item, carries its tooltip, and is outlined while held.
    func show() throws {
        for slot in 0..<Self.slotCount {
            let item = slots[slot].map { Item.catalogue[$0] }
            try widget(slot).setText(item?.glyph ?? "")
            try widget(slot).setTooltip(item.map { "\($0.name): \($0.description)" } ?? "")
            try widget(slot).setClasses(held == slot ? "slot held" : "slot")
        }
    }

    /// The details panel: what is in `slot`.
    func describe(_ slot: Int) throws {
        guard let index = slots[slot] else {
            try document["item-name"].setText("Empty slot")
            try document["item-description"].setText("Nothing here yet.")
            return
        }
        try document["item-name"].setText(Item.catalogue[index].name)
        try document["item-description"].setText(Item.catalogue[index].description)
    }

    /// Accept on a slot: pick its item up, or put the held one down here.
    mutating func accept(_ slot: Int) throws {
        if dragEnded {
            dragEnded = false
            return
        }
        if let from = held {
            slots.swapAt(from, slot)
            held = nil
            drops += 1
        } else if slots[slot] != nil {
            held = slot
            picks += 1
        }
        try show()
        try describe(slot)
    }

    /// Cancel: the held item stays where it was. True when something was held.
    mutating func cancel() throws -> Bool {
        guard held != nil else { return false }
        held = nil
        try show()
        return true
    }

    /// The pointer, once a frame, in reference units.
    mutating func pointer(_ pointer: Pointer, scale: Float) throws {
        dragEnded = false
        let at = Vec2(x: pointer.position.x / scale, y: pointer.position.y / scale)
        if pointer.pressed.contains(.left), let slot = try slot(under: at), slots[slot] != nil {
            press = (slot, at)
        }
        if let started = press, pointer.buttons.contains(.left) {
            let moved = abs(at.x - started.at.x) + abs(at.y - started.at.y)
            if ghost == nil && moved > 6 {
                ghost = try makeGhost(for: started.slot)
            }
            try ghost?.setLayout(.at(x: at.x - 30, y: at.y - 14, width: 60, height: 28))
        }
        if pointer.released.contains(.left), let started = press {
            press = nil
            if let ghost {
                try ghost.destroy()
                self.ghost = nil
                dragEnded = true
                if let target = try slot(under: at), target != started.slot {
                    slots.swapAt(started.slot, target)
                    drags += 1
                }
                try show()
            }
        }
    }

    private func slot(under point: Vec2) throws -> Int? {
        for slot in 0..<Self.slotCount where try widget(slot).element.rect.contains(point) {
            return slot
        }
        return nil
    }

    private func makeGhost(for slot: Int) throws -> UIElement {
        let ghost = try UI.create(.label, in: UI.root(), name: "drag-ghost")
        try ghost.setStyle(UIStyle(background: 0xE6E8_B64C, cornerRadius: 6))
        try ghost.setText(slots[slot].map { Item.catalogue[$0].glyph } ?? "", colour: 0xFF10_1010)
        return ghost
    }

    /// The first slot's item for the report, and where the potion is.
    var firstSlotItem: Float { Float(slots[0] ?? -1) }
    var potionSlot: Float { Float(slots.firstIndex(of: Item.potion) ?? -1) }
}
