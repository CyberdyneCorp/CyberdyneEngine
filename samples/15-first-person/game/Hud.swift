// SPDX-License-Identifier: MIT
// Hud.swift — what the player sees over the arena when they play it: the crosshair at the screen's
// centre, where every shot goes; a hit marker that flashes on a target hit and reddens on a kill;
// the magazine and the reload; what has been hit; and the controls.
//
// ABI 1.6's elements, under the screen's root, all of them the game's: the host draws the store and
// decides nothing about what is in it. A run with no interface — the scripted session, a headless
// one — has no root, so `FirstPersonHud(owner:)` throws and the game plays on without it.

import CyberdyneKit

final class FirstPersonHud {
    /// What the HUD shows, from the game's own state.
    struct Reading {
        var ammo: Float
        var magazine: Float
        var reloading: Bool
        var targetHits: Float
        var kills: Float
    }

    private let crosshair: [UIElement]
    private let marker: [UIElement]
    private let ammo: UIElement
    private let reload: UIElement
    private let score: UIElement
    private var markerLeft: Float = 0
    private var shown = true
    private var last: Reading?

    /// How long the hit marker stays, seconds.
    private static let markerSeconds: Float = 0.22
    private static let white: UIColour = 0xF0F0_F0F0
    private static let hit: UIColour = 0xFFFF_FFFF
    private static let kill: UIColour = 0xFFE6_3B2E

    init(owner: Entity) throws {
        let root = try UI.root()
        func box(_ x: Float, _ y: Float, _ w: Float, _ h: Float, _ colour: UIColour) throws
            -> UIElement
        {
            let element = try UI.create(.panel, in: root, owner: owner)
            try element.setLayout(
                .anchored(
                    min: Vec2(x: 0.5, y: 0.5), max: Vec2(x: 0.5, y: 0.5),
                    offsetMin: Vec2(x: x, y: y), offsetMax: Vec2(x: x + w, y: y + h)))
            try element.setStyle(UIStyle(background: colour))
            return element
        }
        // THE CROSSHAIR: four ticks with a gap the size of the spread a kick opens, and a dot.
        crosshair = [
            try box(-14, -1, 9, 2, Self.white), try box(5, -1, 9, 2, Self.white),
            try box(-1, -14, 2, 9, Self.white), try box(-1, 5, 2, 9, Self.white),
            try box(-1, -1, 2, 2, Self.white),
        ]
        // THE HIT MARKER: four short diagonals, drawn as steps of squares.
        var parts: [UIElement] = []
        for (dx, dy) in [(-1, -1), (1, -1), (-1, 1), (1, 1)] as [(Float, Float)] {
            for step in 0..<3 {
                let reach = 7 + Float(step) * 2
                parts.append(try box(dx * reach - 1.5, dy * reach - 1.5, 3, 3, Self.hit))
            }
        }
        marker = parts
        func label(_ min: Vec2, _ offsetMin: Vec2, _ offsetMax: Vec2) throws -> UIElement {
            let element = try UI.create(.label, in: root, owner: owner)
            try element.setLayout(
                .anchored(min: min, max: min, offsetMin: offsetMin, offsetMax: offsetMax))
            return element
        }
        ammo = try label(Vec2(x: 1, y: 1), Vec2(x: -230, y: -70), Vec2(x: -24, y: -24))
        reload = try label(Vec2(x: 1, y: 1), Vec2(x: -230, y: -104), Vec2(x: -24, y: -76))
        score = try label(Vec2(x: 0, y: 1), Vec2(x: 24, y: -52), Vec2(x: 420, y: -24))
        let help = try label(Vec2(x: 0, y: 0), Vec2(x: 16, y: 12), Vec2(x: 900, y: 34))
        try help.setText(
            "WASD move  Shift sprint  Space jump  Ctrl/C crouch  LMB fire  RMB aim  R reload  "
                + "Esc pause", colour: 0xC0D8_DCE4)
        for part in marker {
            try part.setShown(false)
        }
    }

    /// A shot landed on a target: the marker, white, or red for the shot that put it down.
    func hit(kill: Bool) throws {
        markerLeft = Self.markerSeconds
        for part in marker {
            try part.setStyle(UIStyle(background: kill ? Self.kill : Self.hit))
            try part.setShown(true)
        }
    }

    /// One frame: the marker's time, the counters, and everything hidden while paused.
    func update(_ reading: Reading, seconds: Float, paused: Bool) throws {
        if markerLeft > 0 {
            markerLeft -= seconds
            if markerLeft <= 0 {
                for part in marker {
                    try part.setShown(false)
                }
            }
        }
        if shown == paused {
            shown = !paused
            for part in crosshair {
                try part.setShown(shown)
            }
        }
        guard reading.ammo != last?.ammo || reading.reloading != last?.reloading
            || reading.targetHits != last?.targetHits || reading.kills != last?.kills
        else { return }
        last = reading
        let low = reading.ammo <= reading.magazine * 0.2
        try ammo.setText(
            "\(Int(reading.ammo)) / \(Int(reading.magazine))",
            colour: low ? Self.kill : Self.white, scale: 2)
        try reload.setText(reading.reloading ? "RELOADING" : "", colour: 0xFFFF_D34D)
        try score.setText(
            "Hits \(Int(reading.targetHits))   Down \(Int(reading.kills))", colour: Self.white,
            scale: 2)
    }
}
