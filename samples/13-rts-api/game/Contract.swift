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
    /// ABI 1.10: the interface atlas page an embedder that draws the HUD uploads the fog of war's
    /// minimap overlay to (`fog_of_war::compose_minimap_fog`). Page 1 is the glyphs'.
    static let fogPage: UInt32 = 2
    /// ABI 1.16: the node the host places in the level when there is a battle to fight, and the
    /// prefab a soldier is spawned from.
    static let battlefield = "/Level/Battlefield"
    /// `--play`: the node the host places beside the battlefield when a person plays the battle
    /// (Skirmish.swift), and the one it adds when it replays a recorded game's orders instead.
    static let skirmish = "/Level/Skirmish"
    static let skirmishReplay = "/Level/SkirmishReplay"
    static let soldier = "units/soldier"
    /// The battle's effects (ABI 1.15) and cues, as host/battle_effects.h and host/battle_host.h
    /// register them.
    static let launchEffect = "battle.launch"
    static let trailEffect = "battle.trail"
    static let sparksEffect = "battle.sparks"
    static let explosionEffect = "battle.explosion"
    static let smokeEffect = "battle.smoke"
    static let clashCue = "battle.clash"
    static let bowCue = "battle.bow"
    static let thumpCue = "battle.throw"
    static let boomCue = "battle.boom"
    static let fallCue = "battle.fall"
}

/// The minimap's square, in the level's metres: the level alone, or — with a battle — the RTS test
/// map's whole terrain (`battle::kMapMinX`, `kMapMinZ`, `kMapSize`), which the host draws its
/// topographic minimap over.
struct MinimapFrame {
    var minX: Float
    var minZ: Float
    var size: Float

    static let frontier = MinimapFrame(minX: -48, minZ: -64, size: 128)

    func x(_ worldX: Float) -> Float { (worldX - minX) / size }
    func y(_ worldZ: Float) -> Float { (worldZ - minZ) / size }
}

/// ABI 1.10: the enemy camp the host placed in the level — `/Level/RaiderNear`, `/Level/RaiderFar`
/// and `/Level/Watchtower` — and where each stands, which host/fog.h's `kCamp` states too. What each
/// sees, and what blue is shown of it, is Fog.swift's.
enum Camp {
    /// West of the squad, in its sight from the first frame; and north-east, out of it until the
    /// ordered worker reaches its point.
    static let raiders = [Vec3(x: 8, y: 0, z: 13), Vec3(x: 27, y: 0, z: 29)]
    /// East of the worker that is ordered away: seen at the start, a ghost once it has gone.
    static let tower = Vec3(x: 25, y: 0, z: 13)
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
    /// ABI 1.9 (Lighting.swift). Volumes the GI held after the base's was placed; the luminance at
    /// the beacon's foot lit, put out and lit again; and the calls a fixed step had refused.
    var giVolumes: Float = 0
    var giLit: Float = 0
    var giDark: Float = 0
    var giRelit: Float = 0
    var giRefused: Float = 0
    /// ABI 1.10 (Fog.swift). A worker's reveal radius read back, the cells blue sees, the tower
    /// seen and now a ghost, the raiders blue sees, the far raider hidden on the first frame, the
    /// camp's minimap dots, and the visions a frame was refused.
    var fogRadius: Float = 0
    var fogCells: Float = 0
    var fogTowerSeen: Float = 0
    var fogTowerGhost: Float = 0
    var fogRaidersSeen: Float = 0
    var fogFarHidden: Float = 0
    var fogEnemyDots: Float = 0
    var fogRefused: Float = 0
    /// ABI 1.16 (Battle.swift). Soldiers enlisted, orders given, events read, points of damage the
    /// events reported, deaths, buildings fallen, events shown and hidden by blue's fog, and the
    /// effects, lights, cues and trail marks played.
    var battleSoldiers: Float = 0
    var battleOrders: Float = 0
    var battleEvents: Float = 0
    var battleDamage: Float = 0
    var battleDeaths: Float = 0
    var battleFallen: Float = 0
    var battleShown: Float = 0
    var battleHidden: Float = 0
    var battleEffects: Float = 0
    var battleLights: Float = 0
    var battleSounds: Float = 0
    var battleTrails: Float = 0
    /// `--play` (Skirmish.swift). The player's commands by kind — moves, attack-moves, attacks
    /// and stops, one a unit — and the opponent's; the units selected now and the times the
    /// selection changed; control groups kept and called; minimap jumps; menus opened, pauses
    /// toggled and frames paused; the camera's focus, distance and heading (in turns); and the
    /// outcome (1 victory, 2 defeat) and the tick it was decided on.
    var skirmishMoves: Float = 0
    var skirmishAttackMoves: Float = 0
    var skirmishAttacks: Float = 0
    var skirmishStops: Float = 0
    var skirmishOpponentOrders: Float = 0
    var skirmishPosture: Float = 0
    var skirmishSelected: Float = 0
    var skirmishSelections: Float = 0
    var skirmishAssigned: Float = 0
    var skirmishRecalled: Float = 0
    var skirmishMinimapJumps: Float = 0
    var skirmishMenus: Float = 0
    var skirmishPauses: Float = 0
    var skirmishPausedFrames: Float = 0
    var skirmishFocusX: Float = 0
    var skirmishFocusZ: Float = 0
    var skirmishDistance: Float = 0
    var skirmishHeading: Float = 0
    var skirmishOutcome: Float = 0
    var skirmishOutcomeTick: Float = 0
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
