// SPDX-License-Identifier: MIT
// Hero.swift — the third-person hero: moves, runs, jumps, climbs, and is heard.
//
//   onCreate       (no phase)    a character controller on this node's entity, and an animator
//                                over the host's `hero` rig
//   onReady        (the pump)    `@Node("../Camera")` and the terrain's node are resolved
//   onFixedUpdate  (fixed step)  the player's stick in the camera's frame, accelerated, turned
//                                into a facing and a velocity; a jump with coyote time; the
//                                locomotion blend space's speed, foot placement's weight, and the
//                                jump, fall and land states played
//   onUpdate       (frame)       the clips' footfalls heard on the surface underfoot; the feet
//                                and the camera read for the report
//
// Every number in this file is the game's. The engine sweeps the capsule, climbs the stairs,
// evaluates the blend space, plants the feet on what the physics world says is under them and
// mixes the sound; the host builds the level and draws it.

import CyberdyneKit

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(ucrt)
import ucrt
#endif

@Behaviour(name: "Hero", schema: 1)
final class Hero: Behaviour {

    // --- Tunables ------------------------------------------------------------------------------

    /// Where the capsule's centre starts: on the courtyard's flagstones, south of the stairs.
    @Export var spawn: Vec3 = Vec3(x: 0, y: 0.95, z: 9)
    @Export var spawnFacing: Float = 0
    /// The capsule. Total height including both caps.
    @Export(range: 0.1...1) var radius: Float = 0.3
    @Export(range: 0.5...3) var height: Float = 1.8
    /// The tallest step the controller lifts the hero onto rather than blocking it: a 60 cm wall is
    /// not one. (An 18 cm riser the capsule's round bottom half rides on its own.) A gameplay
    /// decision, made here.
    @Export(range: 0...1) var stepOffset: Float = 0.35
    /// Above this a surface is a wall: the hill's 20-degree flank is walkable.
    @Export(range: 0...1.5) var maxSlope: Float = 0.87

    /// Metres a second: a walk, and a run with the sprint button held.
    @Export(range: 0...10) var walkSpeed: Float = 1.8
    @Export(range: 0...15) var runSpeed: Float = 5.5
    /// How fast the hero gets up to speed and comes to a stop, metres a second per second.
    @Export(range: 0...100) var acceleration: Float = 9
    @Export(range: 0...100) var deceleration: Float = 14
    /// Seconds for the facing to close half the angle to the direction of travel.
    @Export(range: 0...1) var turnHalfLife: Float = 0.06
    @Export(range: 0...20) var jumpSpeed: Float = 5.2
    /// How long after leaving the ground a jump is still allowed.
    @Export(range: 0...0.5) var coyoteSeconds: Float = 0.12
    /// How long the landing pose plays before the locomotion space takes over again.
    @Export(range: 0...1) var landSeconds: Float = 0.22

    @Node("../Camera") var cameraNode: Entity?

    // --- State ---------------------------------------------------------------------------------

    private var velocity = Vec3()
    private var facing: Float = 0
    private var airborne: Float = 0
    private var wasGrounded = true
    private var landing: Float = 0
    private var playing = ""
    private var terrain = Entity.null
    private var groundEntity = Entity.null
    private var tally = HeroTally()

    private var pose = ComponentType(id: 0)
    private var report = ComponentType(id: 0)
    private var aim = ComponentType(id: 0)
    private let poseFields = PoseFields()
    private let reportFields = ReportFields()
    private let aimYaw = CameraAim.field("yaw")

    private var animator: Animator { Animator(entity) }
    private var character: CharacterController { CharacterController(entity) }

    // --- Lifecycle -----------------------------------------------------------------------------

    override func onCreate() throws {
        guard let world else { throw CyberdyneError.invalidHandle }
        pose = try Components.register(HeroPose.self, in: world)
        report = try Components.register(HeroReport.self, in: world)
        aim = try Components.register(CameraAim.self, in: world)
        for component in [pose, report] where !world.has(component, on: entity) {
            try world.add(component, to: entity)
        }
        facing = spawnFacing
        try CharacterController.create(
            on: entity,
            CharacterController.Description(
                radius: radius, height: height, maxSlopeRadians: maxSlope, stepOffset: stepOffset,
                start: Pose(position: spawn)))
        try Animator.attach(to: entity, rig: Content.rig)
        try play(Content.locomotion, crossfade: 0)
        try animator.set(Content.feet, to: Float(1))
        try publish()
    }

    override func onReady() throws {
        terrain = SceneTree.find(Content.terrain) ?? .null
    }

    override func onFixedUpdate(_ delta: Double) throws {
        let step = Float(delta)
        let input = try Input.action(Content.move).axis2
        let sprinting = try Input.action(Content.sprint).pressed
        let jumpPressed = try Input.action(Content.jumpAction).pressCount > 0
        let before = try character.state

        // THE STICK IN THE CAMERA'S FRAME: forward is where the camera looks, flattened.
        let yaw = try cameraYaw()
        let forward = Vec3(x: -sinf(yaw), y: 0, z: -cosf(yaw))
        let right = Vec3(x: cosf(yaw), y: 0, z: -sinf(yaw))
        var wanted = Vec3(
            x: forward.x * input.y + right.x * input.x, y: 0,
            z: forward.z * input.y + right.z * input.x)
        let amount = min((wanted.x * wanted.x + wanted.z * wanted.z).squareRoot(), 1)
        if amount > 1e-3 {
            let scale = 1 / (wanted.x * wanted.x + wanted.z * wanted.z).squareRoot()
            wanted = Vec3(x: wanted.x * scale, y: 0, z: wanted.z * scale)
        }
        let targetSpeed = amount * (sprinting ? runSpeed : walkSpeed)
        accelerate(
            toward: Vec3(x: wanted.x * targetSpeed, y: 0, z: wanted.z * targetSpeed),
            step: step, grounded: before.isGrounded)

        // FACE WHERE THE HERO GOES, turning by a half-life rather than snapping.
        let ground = (velocity.x * velocity.x + velocity.z * velocity.z).squareRoot()
        if ground > 0.2 {
            let heading = atan2f(-velocity.x, -velocity.z)
            facing += wrap(heading - facing) * (1 - exp2f(-step / max(turnHalfLife, 1e-3)))
            facing = wrap(facing)
        }

        // JUMP, with coyote time.
        airborne = before.isGrounded ? 0 : airborne + step
        let jumping = jumpPressed && airborne <= coyoteSeconds
        if jumping {
            airborne = coyoteSeconds + step
            tally.jumps += 1
            try Audio.play(Content.jumpCue, at: before.position)
        }
        try character.move(velocity: velocity, jump: jumping ? jumpSpeed : nil)
        let after = try character.state
        groundEntity = after.groundEntity
        try animate(after, jumping: jumping, step: step)
        try observe(after, step: step)
        try publish()
    }

    override func onUpdate(_ delta: Double) throws {
        // FOOTFALLS, from the clips the blend space is playing: the surface under the hero decides
        // which step is heard, and the foot that fell decides where.
        for event in try Animation.events(for: entity) where event.name == Content.footstep {
            let foot = event.parameter < 0.5 ? "l_toe" : "r_toe"
            let at = try animator.jointPose(foot).position
            let stone = groundEntity != terrain
            let voice = try Audio.play(stone ? Content.stepStone : Content.stepGrass, at: at)
            tally.footsteps += 1
            if stone {
                tally.stoneSteps += 1
            } else {
                tally.grassSteps += 1
            }
            tally.dropped += voice == nil ? 1 : 0
        }
        let feet = try animator.footPlacement()
        if feet.solved {
            tally.feetSolved += 1
            if max(abs(feet.left.offset), abs(feet.right.offset)) > 0.04 {
                tally.feetAdjusted += 1
            }
            tally.pelvisDrop = max(tally.pelvisDrop, -feet.pelvis)
        }
        try publish()
    }

    // --- The parts -----------------------------------------------------------------------------

    private func cameraYaw() throws -> Float {
        guard let world, let cameraNode, world.has(aim, on: cameraNode) else { return facing }
        return try world.float(cameraNode, aim, field: aimYaw)
    }

    /// Ease the horizontal velocity toward `target`: quicker to stop than to start, and with less
    /// grip in the air.
    private func accelerate(toward target: Vec3, step: Float, grounded: Bool) {
        let dx = target.x - velocity.x
        let dz = target.z - velocity.z
        let gap = (dx * dx + dz * dz).squareRoot()
        let speeding =
            target.x * target.x + target.z * target.z
            >= velocity.x * velocity.x + velocity.z * velocity.z
        let rate = (speeding ? acceleration : deceleration) * (grounded ? 1 : 0.35)
        let reach = rate * step
        if gap <= reach || gap < 1e-5 {
            velocity = target
        } else {
            velocity = Vec3(
                x: velocity.x + dx / gap * reach, y: 0, z: velocity.z + dz / gap * reach)
        }
    }

    /// The program's states and parameters for this tick.
    private func animate(_ state: CharacterController.State, jumping: Bool, step: Float) throws {
        let speed = (state.velocity.x * state.velocity.x + state.velocity.z * state.velocity.z)
            .squareRoot()
        try animator.set(Content.speed, to: state.isGrounded ? speed : min(speed, walkSpeed))
        // Feet are placed on the ground the hero stands on, and let go of it in the air — not on
        // the tick or two a step down the stairs leaves the capsule off the tread.
        let planted = state.isGrounded || airborneFor < 0.15
        try animator.set(Content.feet, to: Float(planted ? 1 : 0))
        if jumping {
            try play(Content.jump, crossfade: 0.08)
        } else if !state.isGrounded && state.velocity.y < -1.0 && playing != Content.fall {
            try play(Content.fall, crossfade: 0.25)
        } else if state.isGrounded && !wasGrounded && airborneFor > 0.2 {
            landing = landSeconds
            tally.landings += 1
            try Audio.play(Content.landCue, at: state.position)
            try play(Content.land, crossfade: 0.06)
        } else if state.isGrounded && playing != Content.locomotion {
            landing -= step
            if landing <= 0 {
                try play(Content.locomotion, crossfade: 0.2)
            }
        }
        airborneFor = state.isGrounded ? 0 : airborneFor + step
        wasGrounded = state.isGrounded
    }

    private var airborneFor: Float = 0

    private func play(_ state: String, crossfade: Float) throws {
        try animator.play(state, crossfade: crossfade)
        playing = state
        let bit: Float
        switch state {
        case Content.locomotion: bit = 1
        case Content.jump: bit = 2
        case Content.fall: bit = 4
        default: bit = 8
        }
        if Int(tally.states) & Int(bit) == 0 {
            tally.states += bit
        }
    }

    private func observe(_ state: CharacterController.State, step: Float) throws {
        tally.ticks += 1
        if state.isGrounded {
            tally.groundedTicks += 1
        } else {
            tally.airborneTicks += 1
        }
        tally.steppedUp += state.steppedUp ? 1 : 0
        let feet = state.position.y - height * 0.5
        tally.highest = max(tally.highest, feet)
        let speed = (state.velocity.x * state.velocity.x + state.velocity.z * state.velocity.z)
            .squareRoot()
        tally.fastest = max(tally.fastest, speed)
        if speed > walkSpeed + 0.5 {
            tally.runTicks += 1
        } else if speed > 0.5 {
            tally.walkTicks += 1
        }
    }

    private func publish() throws {
        guard let world else { return }
        try world.setFloat(facing, entity, pose, field: poseFields.facing)
        try world.setFloat(
            (velocity.x * velocity.x + velocity.z * velocity.z).squareRoot(), entity, pose,
            field: poseFields.speed)
        for (field, value) in tally.values(reportFields) {
            try world.setFloat(value, entity, report, field: field)
        }
    }
}

/// An angle wrapped into (−π, π].
func wrap(_ angle: Float) -> Float {
    var wrapped = angle
    while wrapped > Float.pi {
        wrapped -= 2 * Float.pi
    }
    while wrapped <= -Float.pi {
        wrapped += 2 * Float.pi
    }
    return wrapped
}

private struct PoseFields {
    let facing = HeroPose.field("facing")
    let speed = HeroPose.field("speed")
}

/// `HeroReport`'s field indices, resolved by name once.
private struct ReportFields {
    let ticks = HeroReport.field("ticks")
    let groundedTicks = HeroReport.field("groundedTicks")
    let airborneTicks = HeroReport.field("airborneTicks")
    let steppedUp = HeroReport.field("steppedUp")
    let jumps = HeroReport.field("jumps")
    let landings = HeroReport.field("landings")
    let footsteps = HeroReport.field("footsteps")
    let stoneSteps = HeroReport.field("stoneSteps")
    let grassSteps = HeroReport.field("grassSteps")
    let highest = HeroReport.field("highest")
    let fastest = HeroReport.field("fastest")
    let runTicks = HeroReport.field("runTicks")
    let walkTicks = HeroReport.field("walkTicks")
    let feetSolved = HeroReport.field("feetSolved")
    let feetAdjusted = HeroReport.field("feetAdjusted")
    let pelvisDrop = HeroReport.field("pelvisDrop")
    let states = HeroReport.field("states")
    let dropped = HeroReport.field("dropped")
}

/// What the hero counted.
private struct HeroTally {
    var ticks: Float = 0
    var groundedTicks: Float = 0
    var airborneTicks: Float = 0
    var steppedUp: Float = 0
    var jumps: Float = 0
    var landings: Float = 0
    var footsteps: Float = 0
    var stoneSteps: Float = 0
    var grassSteps: Float = 0
    var highest: Float = 0
    var fastest: Float = 0
    var runTicks: Float = 0
    var walkTicks: Float = 0
    var feetSolved: Float = 0
    var feetAdjusted: Float = 0
    var pelvisDrop: Float = 0
    var states: Float = 0
    var dropped: Float = 0

    func values(_ fields: ReportFields) -> [(UInt32, Float)] {
        [
            (fields.ticks, ticks), (fields.groundedTicks, groundedTicks),
            (fields.airborneTicks, airborneTicks), (fields.steppedUp, steppedUp),
            (fields.jumps, jumps), (fields.landings, landings), (fields.footsteps, footsteps),
            (fields.stoneSteps, stoneSteps), (fields.grassSteps, grassSteps),
            (fields.highest, highest), (fields.fastest, fastest), (fields.runTicks, runTicks),
            (fields.walkTicks, walkTicks), (fields.feetSolved, feetSolved),
            (fields.feetAdjusted, feetAdjusted), (fields.pelvisDrop, pelvisDrop),
            (fields.states, states), (fields.dropped, dropped),
        ]
    }
}
