// SPDX-License-Identifier: MIT
// CameraDirector.swift — the third-person camera, as a game decision.
//
//   onReady   (the pump)  `@Node("../Hero")` is resolved; where the host offers an interface,
//                         the pause menu and the controls' line
//   onUpdate  (frame)     the player's look stick — or the mouse, when played — turns the camera;
//                         while the hero moves and the look is still, the camera swings in behind
//                         it; the faster the hero goes the further back it pulls; and all of it is
//                         asked of the engine's camera with `Camera.setTarget` — a focus, a yaw, a
//                         pitch, a distance
//
// PLAYED (samples/play): the mouse's counts orbit the camera at `mouseRadians` a count, scaled by
// the sensitivity the pause menu sets — the one place it is applied — and the focus is the hero
// at the frame's own time between two ticks. Paused, nothing moves and the menu shows. A scripted
// run has no mouse, no menu and frames on their ticks, so none of it changes what it does.
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
    /// Radians a mouse count turns the camera, before the player's sensitivity.
    @Export(range: 0...0.02) var mouseRadians: Float = 0.0025

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
    private var settings = PlaySettings()
    private var menu: PauseMenu?
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

    override func onReady() throws {
        // PLAYED ONLY: a host with no interface — the scripted run — refuses both.
        menu = try? PauseMenu(for: self, title: "Paused") { [unowned self] settings in
            self.settings = settings
        }
        if let root = try? UI.root(), let help = try? UI.create(.label, in: root, owner: entity) {
            try? help.setLayout(
                .anchored(
                    min: Vec2(x: 0, y: 0), max: Vec2(x: 0, y: 0), offsetMin: Vec2(x: 16, y: 12),
                    offsetMax: Vec2(x: 900, y: 34)))
            try? help.setText(
                "WASD move  Shift run  Space jump  Mouse orbit  Esc pause", colour: 0xC0D8_DCE4)
        }
    }

    override func onUpdate(_ delta: Double) throws {
        let now = try Time.now
        try menu?.update(paused: now.isPaused)
        guard !now.isPaused, let world, let hero, world.has(pose, on: hero),
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

        // THE STICK — or the mouse — turns the camera; a still look hands the yaw back to the
        // hero's heading.
        let look = try Input.action(Content.look).axis2
        let mouse = try mouseCounts()
        let stickMoved = abs(look.x) > 0.05 || abs(look.y) > 0.05
        let mouseMoved = mouse.x != 0 || mouse.y != 0
        if stickMoved || mouseMoved {
            if stickMoved {
                yaw = wrap(yaw - look.x * lookSpeed * step)
                pitch = min(max(pitch + look.y * lookSpeed * step, minPitch), maxPitch)
            }
            if mouseMoved {
                // Pulled toward the player — the window's +y — looks down, unless inverted.
                let rate = mouseRadians * settings.lookSensitivity
                let rise: Float = settings.invertY ? 1 : -1
                yaw = wrap(yaw - mouse.x * rate)
                pitch = min(max(pitch + mouse.y * rate * rise, minPitch), maxPitch)
            }
            idle = 0
        } else {
            idle += step
            if idle > followDelay && speed > 0.5 {
                yaw = wrap(yaw + wrap(facing - yaw) * (1 - exp2f(-step / followHalfLife)))
            }
        }
        let wanted = nearDistance + (farDistance - nearDistance) * min(speed / farAtSpeed, 1)
        distance += (wanted - distance) * (1 - exp2f(-step / max(distanceHalfLife, 1e-3)))

        // THE FOCUS AT THE FRAME'S OWN TIME: between two ticks the hero is carried forward by its
        // velocity over that much of a step, as the host draws it. On a tick, where it is.
        let state = try CharacterController(hero).state
        var at = state.position
        let ahead = Float(now.interpolation * now.fixedDelta)
        if ahead > 0 {
            let moved = state.velocity
            at = Vec3(x: at.x + moved.x * ahead, y: at.y + moved.y * ahead, z: at.z + moved.z * ahead)
        }
        try camera.setTarget(
            CameraTarget(
                focus: .position(Vec3(x: at.x, y: at.y + focusHeight, z: at.z)), yaw: yaw,
                pitch: pitch, distance: distance, blendSeconds: cut ? 0 : blendSeconds))
        try world.setFloat(yaw, entity, aim, field: aimYaw)
        try publish(hero)
    }

    /// The mouse's counts this frame, window pixels: zero with no mouse — the scripted run's
    /// keyboard is its only device.
    private func mouseCounts() throws -> Vec2 {
        let pointer = try Input.pointer()
        return pointer.isPresent ? pointer.delta : Vec2(x: 0, y: 0)
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
