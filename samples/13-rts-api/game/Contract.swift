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
    /// The worker's animation rig, which the host cooked and registered under this name, and the
    /// three states its program has: idle, walking, and a one-shot cheer.
    static let workerRig = "worker"
    static let idle = "idle"
    static let walk = "walk"
    static let cheer = "cheer"
    /// The events the worker's clips fire: a footfall, and the cheer finishing.
    static let footstep: AnimationName = "footstep"
    static let cheerDone: AnimationName = "cheer_done"
}

/// The host puts the ground on collision layer 0 and every navigation agent's body on layer 1. A
/// filter's mask selects layers, so a click can ask for a unit or for the ground and get only that.
enum Layers {
    static let ground = Physics.Filter(mask: 1 << 0)
    static let units = Physics.Filter(mask: 1 << 1)
}

/// How long a unit has served, in fixed ticks. Written by the `trainUnits` system — scheduled by
/// the engine, not called by any behaviour — and read by nothing in the game; the host reports it.
@Component
struct Veterancy {
    var ticks: Float = 0
}

/// What the scout did, written by `Scout` for the host's report and nothing else.
@Component
struct ScoutReport {
    /// The crate's `@Node` resolved at `onReady`: 1, or 0 when it did not.
    var crateFound: Float = 0
    /// The hero character's x, its ground state (`GroundState`), and whether it was ever airborne.
    var heroX: Float = 0
    var heroGround: Float = -1
    var airborne: Float = 0
    /// Impulses applied to the crate, and the crate's speed read back the tick it was kicked.
    var kicks: Float = 0
    var crateSpeed: Float = 0
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
    /// Tree callbacks the engine delivered: `onEnterTree` and `onReady`, once each per attachment.
    var entered: Float = 0
    var readied: Float = 0
    /// The barracks `@Node` resolved at `onReady`: 1, or 0 when it did not.
    var barracksFound: Float = 0
    /// ABI 1.6. Workers built by the HUD's Build button rather than the key, clicks on the HUD the
    /// commander heard in `onUIEvent`, and whether the HUD was mounted at all.
    var hudBuilds: Float = 0
    var hudClicks: Float = 0
    var hud: Float = 0
    /// ABI 1.7. Animation events the game read: footfalls of walking units, and cheers that
    /// finished.
    var footsteps: Float = 0
    var cheerEvents: Float = 0
    /// ABI 1.8. The lockstep company: orders given through `Lockstep.order`, arrivals heard, and
    /// the first unit's authoritative x — a `Fixed` field, written through `component_set_fixed`.
    var companyOrders: Float = 0
    var companyArrivals: Float = 0
    var companyLead: Fixed = .zero
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
