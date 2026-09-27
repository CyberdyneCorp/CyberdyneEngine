// SPDX-License-Identifier: MIT
// Contract.swift — what the game and the host agree on. Names, two collision layers, and one
// component the host only reads.
//
// Compare samples/04-character/game/Contract.swift. That game had eight components, and the host
// read or wrote every one of them each tick, because at ABI 1.0 a Swift game could not reach input,
// physics, the camera or audio itself. At ABI 1.3 it can. The only thing left to agree on is the
// NAMES of the content the host loaded: the prefab, the cue, the actions and the collision layers.
// No component goes from the host to the game, and the one component that goes the other way,
// `RtsReport`, is there so the sample can print what happened and its test can check it. Nothing
// in the host reads it to make a decision.

import CyberdyneKit

/// Content the host registers under these names. Loading the level is the host's job, so it knows
/// the names. What to spawn, where, and what to do with it is decided here.
enum Content {
    /// The worker unit's prefab. The host registers it as resident, so `Spawn.prefab` never loads.
    static let worker = "units/worker"
    /// The cue played when a unit reaches the point it was sent to.
    static let arrived = "unit.arrived"
    /// Keyboard camera panning: a two-axis action on WASD and the arrow keys.
    static let pan = "camera.pan"
    /// The build key.
    static let spawn = "unit.spawn"
}

/// The host puts the ground on collision layer 0 and every navigation agent's body on layer 1. A
/// filter's mask selects layers, so a click can ask for a unit or for the ground and get only that.
enum Layers {
    static let ground = Physics.Filter(mask: 1 << 0)
    static let units = Physics.Filter(mask: 1 << 1)
}

/// What the game did, written by `Commander` for the host's report and nothing else.
@Component
struct RtsReport {
    /// The unit that is selected now, or null.
    var selected: Entity = .null
    /// Move orders handed to navigation.
    var orders: Float = 0
    /// Arrivals seen: a unit's navigation state reported ARRIVED with its one-tick event.
    var arrivals: Float = 0
    /// Arrival cues the audio server accepted.
    var cues: Float = 0
    /// Units built with the spawn key, after the starting squad.
    var spawns: Float = 0
}

extension Component {
    /// A field's index, by name. Resolved once in `onCreate`, never per tick.
    static func field(_ name: String) -> UInt32 {
        guard let index = componentFields.firstIndex(where: { $0.name == name }) else {
            Log.error("\(componentName) has no field named '\(name)'")
            return 0
        }
        return UInt32(index)
    }
}
