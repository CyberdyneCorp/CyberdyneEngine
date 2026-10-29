// SPDX-License-Identifier: MIT
// Contract.swift — what the game and the host agree on: the NAMES of the content the host made
// (host/arena.h states the same ones), the collision layers, and two components.
//
// `Health` goes on each target. The game owns it — adds it at `onReady`, writes it when a shot
// lands — and the engine's AI reads it as what the target FELT (host/targets.h): the game never
// tells a target what to do, it only says it was hit. `PlayerReport` is there so the sample can
// print what happened and its tests can check it; nothing in the host acts on it.

import CyberdyneKit

/// Content the host registered under these names.
enum Content {
    static let move = "player.move"
    static let sprint = "player.sprint"
    static let jump = "player.jump"
    static let crouch = "player.crouch"
    static let fire = "weapon.fire"
    static let aim = "weapon.aim"
    /// The played game's: the pad's look stick and the reload.
    static let look = "player.look"
    static let reload = "weapon.reload"
    static let shot = "weapon.shot"
    static let impact = "weapon.impact"
    static let hit = "target.hit"
    static let down = "target.down"
    static let muzzle = "weapon.muzzle"
    static let sparks = "impact.sparks"
    static let scorch = "impact.scorch"
}

/// The host's collision layers: the arena's boxes 0, props 1, targets 2, the player 3, and the
/// fallen 4, which nothing is shot at.
enum Layers {
    static let world: UInt32 = 0
    static let props: UInt32 = 1
    static let targets: UInt32 = 2
    static let player: UInt32 = 3
    static let solid: UInt32 = (1 << world) | (1 << props) | (1 << targets)
    /// What a shot, a headroom check and the character collide with.
    static let shot = Physics.Filter(layer: player, mask: solid)
}

/// The layer the arms and the weapon are drawn on, which the weapon view draws and the world does
/// not.
let weaponLayers: UInt32 = 1 << 5

/// A target's health, as the game keeps it.
@Component
struct Health {
    var points: Float = 100
    /// Shots that landed, and where the last came from.
    var hits: Float = 0
    var source: Vec3 = Vec3()
}

/// What the player did, written for the host's report and nothing else.
@Component
struct PlayerReport {
    // The walk: the fastest it went on the ground walking, sprinting and crouched, the jumps, the
    // ticks in the air and the highest the feet got, the ticks crouched, the ticks a stand was
    // refused for want of headroom, and the ticks spent crouched under the beam.
    var walkSpeed: Float = 0
    var sprintSpeed: Float = 0
    var crouchSpeed: Float = 0
    var jumps: Float = 0
    var airborne: Float = 0
    var peakHeight: Float = 0
    var crouched: Float = 0
    var standRefused: Float = 0
    var underBeam: Float = 0
    // The weapon.
    var shots: Float = 0
    var targetHits: Float = 0
    var propHits: Float = 0
    var worldHits: Float = 0
    var misses: Float = 0
    var kills: Float = 0
    var impulses: Float = 0
    var kicks: Float = 0
    var aimed: Float = 0
    // What the shots left behind, through ABI 1.11 and 1.15, and the sounds.
    var flashes: Float = 0
    var bursts: Float = 0
    var sparks: Float = 0
    var marks: Float = 0
    var sounds: Float = 0
    /// Presentation calls a fixed step made and was refused: an effect and a mark.
    var refused: Float = 0
    // The magazine: the rounds in it, the reloads finished, the pulls on an empty one, and the
    // targets the game stood back up.
    var ammo: Float = 0
    var reloads: Float = 0
    var dryFires: Float = 0
    var respawns: Float = 0
    // The last shot, whole: the host captures it the tick its count moves.
    var lastOrigin: Vec3 = Vec3()
    var lastDirection: Vec3 = Vec3()
    var lastHit: Entity = .null
    var lastPoint: Vec3 = Vec3()
    var lastNormal: Vec3 = Vec3()
    var lastDistance: Float = -1
    /// 0 a miss, 1 the world, 2 a prop, 3 a target.
    var lastKind: Float = 0
}

extension Component {
    /// A field's index, by name. Resolved once, never per tick.
    static func field(_ name: String) -> UInt32 {
        guard let index = componentFields.firstIndex(where: { $0.name == name }) else {
            Log.error("\(componentName) has no field named '\(name)'")
            return 0
        }
        return UInt32(index)
    }
}
