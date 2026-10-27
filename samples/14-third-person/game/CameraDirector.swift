// SPDX-License-Identifier: MIT
// CameraDirector.swift — the third-person camera, as a game decision.
//
//   onReady   (the pump)  `@Node("../Hero")` is resolved
//   onUpdate  (frame)     the player's look stick turns the camera; while the hero moves and the
//                         stick is still, the camera swings in behind it; the faster the hero
//                         goes the further back it pulls; and all of it is asked of the engine's
//                         camera with `Camera.setTarget` — a focus, a yaw, a pitch, a distance
//
// WHAT THIS DOES NOT DO: place the camera. The camera server's rig turns the request into a pose,
// and its collision node — the host answers its queries against the physics world — pulls the
// camera in front of a wall, a pillar or the stairs' underside that would come between it and the
// hero. This behaviour sees that only by reading the view back, which it does for the report.

import CyberdyneKit

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(ucrt)
import ucrt
#endif

@Behaviour(name: "CameraDirector", schema: 1)
final class CameraDirector: Behaviour {

    // --- Tunables ------------------------------------------------------------------------------

    /// Radians a second at full stick.
    @Export(range: 0...10) var lookSpeed: Float = 2.4
    /// Pitch limits and where the camera starts: negative looks down.
    @Export(range: -1.5...0) var minPitch: Float = -0.95
    @Export(range: -0.5...1.5) var maxPitch: Float = 0.3
    @Export(range: -1.5...1.5) var startPitch: Float = -0.22
    /// The point framed: the hero's head, not its feet.
    @Export(range: 0...3) var focusHeight: Float = 0.65
    /// Metres back at a stand and at a run.
    @Export(range: 0.5...20) var nearDistance: Float = 3.4
    @Export(range: 0.5...20) var farDistance: Float = 4.6
    /// The speed the camera is furthest back at, and how quickly it gets there.
    @Export(range: 0.5...20) var farAtSpeed: Float = 5.5
    @Export(range: 0...2) var distanceHalfLife: Float = 0.5
    /// How long the stick must rest before the camera swings in behind the hero, and how quickly.
    @Export(range: 0...5) var followDelay: Float = 0.6
    @Export(range: 0...5) var followHalfLife: Float = 0.9
    /// What every request after the first asks the rig to blend over: not a cut.
    @Export(range: 0.01...2) var blendSeconds: Float = 0.25

    @Node("../Hero") var hero: Entity?

    // --- State ---------------------------------------------------------------------------------

    private var yaw: Float = 0
    private var pitch: Float = 0
    private var distance: Float = 0
    private var idle: Float = 0
    private var started = false
    private var aim = ComponentType(id: 0)
    private var pose = ComponentType(id: 0)
    private var report = ComponentType(id: 0)
    private var pulled: Float = 0
    private var nearest: Float = 1
    private let aimYaw = CameraAim.field("yaw")
    private let poseFacing = HeroPose.field("facing")
    private let poseSpeed = HeroPose.field("speed")
    private let reportPulled = HeroReport.field("cameraPulled")
    private let reportNearest = HeroReport.field("cameraNearest")
    private let reportYaw = HeroReport.field("cameraYaw")

    override func onCreate() throws {
        guard let world else { throw CyberdyneError.invalidHandle }
        aim = try Components.register(CameraAim.self, in: world)
        pose = try Components.register(HeroPose.self, in: world)
        report = try Components.register(HeroReport.self, in: world)
        if !world.has(aim, on: entity) {
            try world.add(aim, to: entity)
        }
        pitch = startPitch
        distance = nearDistance
    }

    override func onUpdate(_ delta: Double) throws {
        guard let world, let hero, world.has(pose, on: hero),
            let camera = try Camera.active()
        else { return }
        let step = Float(delta)
        let facing = try world.float(hero, pose, field: poseFacing)
        let speed = try world.float(hero, pose, field: poseSpeed)
        // The first request CUTS to the hero; every later one is a move the rig smooths — and a
        // move, not a cut, is what lets its collision ease the camera back out from a wall.
        let cut = !started
        if cut {
            yaw = facing
            started = true
        }
        try observe(camera, hero: hero)

        // THE STICK turns the camera; a still stick hands the yaw back to the hero's heading.
        let look = try Input.action(Content.look).axis2
        if abs(look.x) > 0.05 || abs(look.y) > 0.05 {
            yaw = wrap(yaw - look.x * lookSpeed * step)
            pitch = min(max(pitch + look.y * lookSpeed * step, minPitch), maxPitch)
            idle = 0
        } else {
            idle += step
            if idle > followDelay && speed > 0.5 {
                yaw = wrap(yaw + wrap(facing - yaw) * (1 - exp2f(-step / followHalfLife)))
            }
        }
        let wanted = nearDistance + (farDistance - nearDistance) * min(speed / farAtSpeed, 1)
        distance += (wanted - distance) * (1 - exp2f(-step / max(distanceHalfLife, 1e-3)))

        let at = try CharacterController(hero).state.position
        try camera.setTarget(
            CameraTarget(
                focus: .position(Vec3(x: at.x, y: at.y + focusHeight, z: at.z)), yaw: yaw,
                pitch: pitch, distance: distance, blendSeconds: cut ? 0 : blendSeconds))
        try world.setFloat(yaw, entity, aim, field: aimYaw)
        try publish(hero)
    }

    /// Where the rig put the camera against where this behaviour asked for it, from the view the
    /// frame shows: how often a wall pulled it in, and how far.
    private func observe(_ camera: Camera, hero: Entity) throws {
        let eye = try camera.view().pose.position
        let at = try CharacterController(hero).state.position
        let dx = eye.x - at.x
        let dy = eye.y - (at.y + focusHeight)
        let dz = eye.z - at.z
        let ratio = (dx * dx + dy * dy + dz * dz).squareRoot() / max(distance, 1e-3)
        if ratio < 0.85 {
            pulled += 1
        }
        nearest = min(nearest, ratio)
    }

    private func publish(_ hero: Entity) throws {
        guard let world, world.has(report, on: hero) else { return }
        try world.setFloat(pulled, hero, report, field: reportPulled)
        try world.setFloat(nearest, hero, report, field: reportNearest)
        try world.setFloat(yaw, hero, report, field: reportYaw)
    }
}
