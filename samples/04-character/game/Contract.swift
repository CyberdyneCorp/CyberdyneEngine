// Contract.swift — the eight components the game and the host meet over. Task 5.1.
//
// THIS FILE IS THE WHOLE BOUNDARY. Everything the C++ host knows about this game is here: eight
// component types, their field names, and their field types. It knows no behaviour's code, no
// tunable, no level geometry and no rule.
//
// The direction of each component is stated on it and it is not decoration — it is what makes the
// artefact's claim checkable. Read down the `host ->` list and you have the whole of what the
// engine tells the game; read down the `-> host` list and you have the whole of what the game asks
// the engine to do. There is no third list, and a decision that is not expressed in the second one
// is a decision the host would have had to make, which is the thing this sample exists to avoid.
//
// WHY COMPONENTS AND NOT AN APPENDED ABI ENTRY. ABI 1.0's interface table carries the engine-neutral
// core — diagnostics, values, entities, components, behaviours — and nothing about input, physics,
// audio or cameras; those entries are appended by the subsystems that own them, which is what the
// append-only rule is for and which has not happened yet. So a game reaches those subsystems the way
// any ECS game does: it writes what it wants into components and the host, which is the only thing
// that may name a server, carries them across. When `input`, `physics`, `camera` and `audio` entries
// are appended to `CyInterface`, the components that exist only to carry them — `PlayerInput`,
// `CharacterDrive`, `AudioCue`, `CameraIntent` — become calls, and nothing else in this directory
// changes.
//
// FIELDS ARE MATCHED BY NAME, NEVER BY POSITION. `CyWorld_T::find` hands the host the field records
// a registration produced — name, type, offset — so the host resolves `"velocity"` rather than
// counting to two. Reordering the properties of a struct below is therefore not a breaking change,
// and forgetting to reorder the host's copy is not a silent misread of a neighbouring field.

import CyberdyneKit

// --- host -> game: what the engine tells the game ------------------------------------------------

/// One tick of resolved intent, written by the host from the committed gameplay command stream.
///
/// It is NOT the input device's state. The host reads `cy::input::CommandFrame`, records a command
/// into `cy::gameplay::CommandStream`, commits it, and writes THE COMMITTED COMMAND here — so what
/// a behaviour reads is what the simulation consumed, and a replay of the log reproduces it. See
/// the host's bridge.cpp for why there is no second path.
@Component
struct PlayerInput {
    /// Movement intent in the camera's frame: `x` strafes, `z` is forward, `y` is unused. Already
    /// normalised to at most unit length by the binding's composite; never scaled by a speed, which
    /// is the game's number and not the engine's.
    var move: Vec3 = Vec3()
    /// Look delta for this tick in radians, `x` yaw and `y` pitch, after the input layer's
    /// sensitivity and inversion. `camera-system` requires those to be applied in one place.
    var look: Vec3 = Vec3()
    /// 1 on a tick in which the jump action went down, whether or not it is still held. A press and
    /// a release inside one tick window both arrive — see `input-and-actions`' fixed-tick sampling.
    var jump: Float = 0
    /// 1 while the sprint action is held at the end of the tick.
    var sprint: Float = 0
}

/// What the character controller did last tick, written by the host after the physics step.
@Component
struct CharacterState {
    var position: Vec3 = Vec3()
    var velocity: Vec3 = Vec3()
    /// 1 when the controller reported `GroundState::Grounded`. A steep slope is not grounded, which
    /// is what lets the game refuse a jump off a wall without knowing what a slope is.
    var grounded: Float = 0
    /// Horizontal speed in metres per second, which is what a footstep cadence is a function of.
    var speed: Float = 0
}

// --- game -> host: what the game asks the engine to do -------------------------------------------

/// The capsule the host builds, written once by `Character.onCreate`.
///
/// The character's *shape* is a gameplay decision — a game whose hero is a metre tall plays
/// differently — so it is declared here rather than in the host, and the host reads it exactly once.
@Component
struct CharacterSpec {
    var spawn: Vec3 = Vec3()
    var radius: Float = 0
    /// Total height including both caps: the number an artist has. See `physics`' character.h.
    var height: Float = 0
    var stepOffset: Float = 0
    var maxSlopeRadians: Float = 0
}

/// One tick of desired motion, written by `Character.onFixedUpdate` and read by the host, which
/// hands it to `cy::physics::CharacterController::move`.
@Component
struct CharacterDrive {
    /// Metres per second in world space, horizontal. The controller integrates gravity itself.
    var velocity: Vec3 = Vec3()
    /// 1 to replace this step's vertical velocity with `jumpSpeed`.
    var jump: Float = 0
    var jumpSpeed: Float = 0
}

/// The camera rig the host builds, written once by `CameraDirector.onCreate`.
///
/// A third-person camera's framing — how far back, how high, how quickly it catches up, how wide
/// the lens is — is a game's decision and not an engine's, so every number the rig is compiled from
/// crosses here. The host chooses the rig's *topology* (target, follow, orbit, look-at, lens) and
/// none of its values.
@Component
struct CameraSpec {
    /// Offset from the focus point, in the rig's own frame, at zoom 0.
    var offset: Vec3 = Vec3()
    var nearDistance: Float = 0
    var farDistance: Float = 0
    /// Seconds for the follow smoother to close half the distance. See `camera-system`' smoothing:
    /// a half-life composes over a variable step and a lerp factor does not.
    var positionHalfLife: Float = 0
    var rotationHalfLife: Float = 0
    /// Vertical field of view in radians at zoom 0 and at zoom 1.
    var nearFieldOfView: Float = 0
    var farFieldOfView: Float = 0
}

/// Where the camera should be pointed this tick, written by `CameraDirector.onFixedUpdate`.
///
/// Not a pose. The game says what it wants framed and how far round the character it is looking; the
/// camera server decides where that puts the camera, which is `camera-system`'s separation of the
/// four concepts and the reason there is no `set_pose()` to write to.
@Component
struct CameraIntent {
    /// The world point to frame — the character's head, not its feet, because the game decides that.
    var focus: Vec3 = Vec3()
    var yaw: Float = 0
    var pitch: Float = 0
    /// Normalised in [0, 1], mapped by the rig onto distance and lens.
    var zoom: Float = 0
}

/// Monotone counters, written by the game and read by the host, which plays a sound each time one
/// advances.
///
/// COUNTERS RATHER THAN FLAGS, and the reason is the tick loop: a frame may contain up to eight
/// fixed ticks, and a flag written on the first and read after the last loses the other seven. A
/// counter loses nothing and needs no agreement about when the host reads it.
@Component
struct AudioCue {
    var footsteps: Float = 0
    var landings: Float = 0
    var jumps: Float = 0
}

/// One static box of the level, written by `Level.onCreate` on an entity of its own.
///
/// The level is content and it is authored here, in the game, for the same reason the character's
/// capsule is: a host that knew where the walls are would be a host with a stake in the game.
@Component
struct LevelBox {
    var center: Vec3 = Vec3()
    var halfExtents: Vec3 = Vec3()
}

// --- Resolving them ------------------------------------------------------------------------------

/// The component types this game uses, registered on demand and remembered per behaviour instance.
///
/// NO GLOBAL CACHE, deliberately. `Components.register` is idempotent by name — cy_abi.h requires it
/// to be, because a reloaded module registers its types again — so each behaviour resolving what it
/// needs in `onCreate` costs one lookup per instance and keeps this module free of the mutable
/// process-wide state that a retired generation would otherwise share with a live one.
struct Registry {
    let world: World

    init(world: World) { self.world = world }

    func type(_ component: any Component.Type) throws -> ComponentType {
        try Components.register(component, in: world)
    }
}

extension Component {
    /// The index of a field, by name, for the typed accessors — which take a position because
    /// `component_get_f32` and its three siblings are the ABI's no-marshalling path and a name
    /// lookup per access would defeat them.
    ///
    /// Resolved ONCE, in `onCreate`, and held in the instance. A behaviour that called this in
    /// `onFixedUpdate` would be doing a linear scan sixty times a second for a number that cannot
    /// change while the module is loaded.
    static func field(_ name: String) -> UInt32 {
        guard let index = componentFields.firstIndex(where: { $0.name == name }) else {
            // Unreachable for a name written in this file, and a crash here would be a crash inside
            // the engine's tick. Zero addresses the first field, and the host's own contract check
            // — which resolves every field by name — is what actually catches a typo, before a
            // frame runs.
            Log.error("\(componentName) has no field named '\(name)'")
            return 0
        }
        return UInt32(index)
    }
}
