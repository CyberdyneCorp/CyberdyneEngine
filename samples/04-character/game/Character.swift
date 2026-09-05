// Character.swift — the third-person character. Task 5.1.
//
// Move, jump, collide, and be heard. Every number in this file is a decision this game makes, and
// none of them exists anywhere in the sample's C++: the host reads `CharacterSpec` to build a
// capsule and `CharacterDrive` to move it, and has no opinion about either.
//
// WHAT THIS BEHAVIOUR DOES NOT DO, AND WHY THAT IS THE POINT. It does not sweep a capsule, resolve a
// contact, or integrate gravity — `cy::physics::CharacterController` does, once, in engine code, so
// that "walking up stairs" means the same thing over the reference backend and over Jolt. It does
// not read a key, because the simulation's only input is the committed command stream. It does not
// place a camera, because a camera is not a scene object. What is left is exactly the game: how fast
// this character walks, when it may jump, how far it travels between footsteps.
//
// THE PRIVATE STATE IS THE HOT-RELOAD DEMONSTRATION. `distanceSinceStep` and `wasGrounded` are not
// `@Export`ed, so a reload reinitialises them and `onAfterReload` is where they are rebuilt from
// what did survive. `swift-scripting` requires exactly that seam, and a behaviour that exported
// everything would never exercise it.

import CyberdyneKit

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#endif

@Behaviour(name: "Character", schema: 1)
final class Character: Behaviour {
    // --- The tunables ----------------------------------------------------------------------------
    //
    // `@Export`ed, so they are inspectable, range-checked where a range means something, and carried
    // across a hot reload BY NAME — the only migration that works (see cy/abi/module.h).

    @Export(range: 0 ... 20) var walkSpeed: Float = 4.5
    @Export(range: 0 ... 20) var sprintSpeed: Float = 7.6
    @Export(range: 0 ... 20) var jumpSpeed: Float = 6.2

    /// The capsule. Total height including both caps, which is the number an artist has.
    @Export(range: 0.1 ... 2) var capsuleRadius: Float = 0.35
    @Export(range: 0.5 ... 4) var capsuleHeight: Float = 1.8
    /// The tallest step climbed rather than blocked. The level below has a 0.3 m stair, so a game
    /// that lowered this to 0.2 would find the stair a wall — which is a gameplay change, made here.
    @Export(range: 0 ... 1) var stepOffset: Float = 0.4
    /// Above this a surface is a wall. 46 degrees, so the level's 30-degree ramp is walkable and its
    /// 60-degree face is not.
    @Export(range: 0 ... 1.5) var maxSlopeRadians: Float = 0.8

    @Export var spawn: Vec3 = Vec3(x: 0, y: 1.4, z: 6)

    /// Metres of ground covered per footstep. A cadence expressed as distance rather than as time is
    /// what makes a sprinting character's steps speed up without a second number.
    @Export(range: 0.2 ... 5) var strideLength: Float = 1.65
    /// How long after leaving the ground a jump is still allowed. Every third-person game has this
    /// number and no engine should.
    @Export(range: 0 ... 0.5) var coyoteSeconds: Float = 0.12

    // --- Private state: reinitialised by a reload, rebuilt in onAfterReload -----------------------

    private var distanceSinceStep: Float = 0
    private var wasGrounded = false
    private var airborneSeconds: Float = 0

    // --- Resolved once, in onCreate ---------------------------------------------------------------

    private var input = ComponentType(id: 0)
    private var state = ComponentType(id: 0)
    private var drive = ComponentType(id: 0)
    private var cue = ComponentType(id: 0)
    private var camera = ComponentType(id: 0)

    private var inputMove: UInt32 = 0
    private var inputJump: UInt32 = 0
    private var inputSprint: UInt32 = 0
    private var stateGrounded: UInt32 = 0
    private var stateSpeed: UInt32 = 0
    private var driveVelocity: UInt32 = 0
    private var driveJump: UInt32 = 0
    private var driveJumpSpeed: UInt32 = 0
    private var cueFootsteps: UInt32 = 0
    private var cueLandings: UInt32 = 0
    private var cueJumps: UInt32 = 0
    private var cameraYaw: UInt32 = 0

    override func onCreate() throws {
        guard let world else { throw CyberdyneError.invalidHandle }
        let registry = Registry(world: world)

        input = try registry.type(PlayerInput.self)
        state = try registry.type(CharacterState.self)
        drive = try registry.type(CharacterDrive.self)
        cue = try registry.type(AudioCue.self)
        camera = try registry.type(CameraIntent.self)
        let spec = try registry.type(CharacterSpec.self)

        inputMove = PlayerInput.field("move")
        inputJump = PlayerInput.field("jump")
        inputSprint = PlayerInput.field("sprint")
        stateGrounded = CharacterState.field("grounded")
        stateSpeed = CharacterState.field("speed")
        driveVelocity = CharacterDrive.field("velocity")
        driveJump = CharacterDrive.field("jump")
        driveJumpSpeed = CharacterDrive.field("jumpSpeed")
        cueFootsteps = AudioCue.field("footsteps")
        cueLandings = AudioCue.field("landings")
        cueJumps = AudioCue.field("jumps")
        cameraYaw = CameraIntent.field("yaw")

        // The four this behaviour owns. `PlayerInput` and `CharacterState` are added here too, and
        // that is deliberate: the host writes them, but the GAME decides that this entity is one
        // that receives input and has a body. An entity is what its components say it is.
        for component in [input, state, drive, cue, spec] where !world.has(component, on: entity) {
            try world.add(component, to: entity)
        }

        try world.setVec3(spawn, entity, spec, field: CharacterSpec.field("spawn"))
        try world.setFloat(capsuleRadius, entity, spec, field: CharacterSpec.field("radius"))
        try world.setFloat(capsuleHeight, entity, spec, field: CharacterSpec.field("height"))
        try world.setFloat(stepOffset, entity, spec, field: CharacterSpec.field("stepOffset"))
        try world.setFloat(maxSlopeRadians, entity, spec,
                           field: CharacterSpec.field("maxSlopeRadians"))

        Log.info("Character spawned at (\(spawn.x), \(spawn.y), \(spawn.z))")
    }

    override func onFixedUpdate(_ delta: Double) throws {
        guard let world else { return }
        let step = Float(delta)

        let move = try world.vec3(entity, input, field: inputMove)
        let wantsJump = try world.float(entity, input, field: inputJump) > 0.5
        let sprinting = try world.float(entity, input, field: inputSprint) > 0.5
        let grounded = try world.float(entity, state, field: stateGrounded) > 0.5
        let travelled = try world.float(entity, state, field: stateSpeed) * step

        // --- Where "forward" is ---------------------------------------------------------------
        //
        // Movement is in the camera's frame, which is what a third-person control scheme means, and
        // the camera's yaw is the camera director's decision — read from its component rather than
        // recomputed here, so the two cannot disagree by a tick. The director is created before this
        // behaviour, so the yaw read here is this tick's.
        let yaw = try world.float(entity, camera, field: cameraYaw)
        let forwardX = -sinf(yaw)
        let forwardZ = -cosf(yaw)
        let speed = sprinting ? sprintSpeed : walkSpeed
        let desired = Vec3(x: (move.x * -forwardZ + move.z * forwardX) * speed,
                           y: 0,
                           z: (move.x * forwardX + move.z * forwardZ) * speed)

        // --- Jump, with coyote time -------------------------------------------------------------
        airborneSeconds = grounded ? 0 : airborneSeconds + step
        let mayJump = grounded || airborneSeconds <= coyoteSeconds
        let jumping = wantsJump && mayJump
        if jumping {
            // Spending the coyote window: without this a held jump re-fires every tick of the window.
            airborneSeconds = coyoteSeconds + step
        }

        try world.setVec3(desired, entity, drive, field: driveVelocity)
        try world.setFloat(jumping ? 1 : 0, entity, drive, field: driveJump)
        try world.setFloat(jumpSpeed, entity, drive, field: driveJumpSpeed)

        // --- What is heard ----------------------------------------------------------------------
        //
        // Cadence is distance, not time: a sprint speeds the steps up without a second tunable, and
        // a character pushed into a wall stops making them because it stops covering ground.
        if grounded {
            distanceSinceStep += travelled
            if distanceSinceStep >= strideLength {
                distanceSinceStep -= strideLength
                try advance(cueFootsteps)
            }
        }
        if grounded && !wasGrounded {
            distanceSinceStep = 0
            try advance(cueLandings)
        }
        if jumping {
            try advance(cueJumps)
        }
        wasGrounded = grounded
    }

    /// `swift-scripting`: private state "SHALL be reinitialised on reload and the behaviour SHALL be
    /// given an `onAfterReload` callback to rebuild it".
    ///
    /// There is nothing to recover `distanceSinceStep` from — it is a phase, not a fact — so the
    /// honest answer is to restart the stride rather than to guess, and to say so once. `wasGrounded`
    /// IS recoverable, from the state the host wrote last tick, so it is recovered.
    override func onAfterReload(restored: Set<String>) throws {
        distanceSinceStep = 0
        airborneSeconds = 0
        if let world {
            wasGrounded = try world.float(entity, state, field: stateGrounded) > 0.5
        }
        Log.info("Character reloaded; restored \(restored.sorted().joined(separator: ", "))")
    }

    private func advance(_ field: UInt32) throws {
        guard let world else { return }
        let count = try world.float(entity, cue, field: field)
        try world.setFloat(count + 1, entity, cue, field: field)
    }
}
