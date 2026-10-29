// SPDX-License-Identifier: MIT
// SkirmishCamera.swift — the camera a player flies over the battlefield: a point on the ground it
// looks at, a heading round that point and a distance from it, all of them the player's.
//
//   pan       the pan keys (WASD and the arrows, `camera.pan`) and the pointer against a screen
//             edge, along the ground in the camera's own frame, faster the further out it is
//   zoom      the mouse wheel, between `nearest` and `farthest` metres
//   rotate    the middle button held and the mouse dragged sideways
//   jump      a click on the minimap, or a control group called twice
//
// The focus is clamped to the battlefield and a margin round it, so the camera never flies off over
// ground nothing happens on. Everything here is presentation: it is decided in `onUpdate` and told
// to the engine as a pose (`Camera.overridePose`), which the rig shows from the next frame. A
// fixed step never reads it.

import CyberdyneKit

struct SkirmishCamera {
    /// The ground the focus may be over, in the level's metres: the battlefield and six metres
    /// round it (`battle::kFieldX`, `kFieldZ`, the company field's 48 metres).
    static let bounds = (minX: Float(-14), maxX: Float(46), minZ: Float(-58), maxZ: Float(2))
    static let nearest: Float = 14
    static let farthest: Float = 64
    /// How steeply the camera looks down: 55 degrees, as its sine and cosine.
    static let pitchSine: Float = 0.819_152
    static let pitchCosine: Float = 0.573_576
    /// A notch of the wheel moves the camera this share of its distance.
    static let zoomStep: Float = 0.12
    /// A pixel of sideways drag turns the camera this much: a turn for 1200 pixels.
    static let turnPerPixel: Float = 4_294_967_296 / 1200
    /// How close to an edge, in pixels, the pointer pans.
    static let edgeBand: Float = 6

    /// The ground point the camera looks at.
    private(set) var focus: Vec3
    /// The heading round it: a binary angle, a whole turn in 2^32. Zero looks down world -z.
    private(set) var heading: Angle
    /// Metres from the focus to the eye.
    private(set) var distance: Float

    init(focus: Vec3, heading: Angle, distance: Float) {
        self.focus = SkirmishCamera.clamped(focus)
        self.heading = heading
        self.distance = min(max(distance, SkirmishCamera.nearest), SkirmishCamera.farthest)
    }

    /// The heading's sine and cosine, for the arithmetic below.
    private var turn: (sin: Float, cos: Float) {
        (Float(Detmath.sin(heading).presentation), Float(Detmath.cos(heading).presentation))
    }

    /// Along the ground, away from the eye: where W moves the focus.
    var ahead: Vec2 { Vec2(x: -turn.sin, y: -turn.cos) }
    /// Along the ground, to the screen's right: where D moves it.
    var right: Vec2 { Vec2(x: turn.cos, y: -turn.sin) }

    /// One frame of the player's hand: keys and edges pan, the wheel zooms, a middle drag turns.
    /// A pointer that has never moved is not a hand at an edge, so `edges` is false until it has.
    mutating func steer(keys: Vec2, pointer: Pointer, edges: Bool, viewport: Vec4, delta: Float) {
        var push = keys
        if edges && pointer.isPresent && pointer.isInWindow {
            push.x += edge(pointer.position.x, origin: viewport.x, extent: viewport.z)
            push.y -= edge(pointer.position.y, origin: viewport.y, extent: viewport.w)
        }
        push = Vec2(x: min(max(push.x, -1), 1), y: min(max(push.y, -1), 1))
        let speed = 0.75 * distance * delta
        move(
            by: Vec2(
                x: (right.x * push.x + ahead.x * push.y) * speed,
                y: (right.y * push.x + ahead.y * push.y) * speed))
        if pointer.wheel.y != 0 {
            let scaled = distance * (1 - SkirmishCamera.zoomStep * pointer.wheel.y)
            distance = min(max(scaled, SkirmishCamera.nearest), SkirmishCamera.farthest)
        }
        if pointer.buttons.contains(.middle) && pointer.delta.x != 0 {
            let turned = Int64(pointer.delta.x * SkirmishCamera.turnPerPixel)
            heading = Angle(raw: heading.raw &+ UInt32(truncatingIfNeeded: turned))
        }
    }

    /// Put the focus over `point` at once: a minimap click, a group called twice.
    mutating func jump(to point: Vec3) {
        focus = SkirmishCamera.clamped(point)
    }

    /// Tell the engine where the eye is and where it looks.
    func show(_ camera: Camera) throws {
        try camera.overridePose(pose)
    }

    /// The eye, and the rotation that looks from it at the focus: the heading about world y, then
    /// the pitch down about the camera's own x.
    var pose: Pose {
        let (sin, cos) = turn
        let eye = Vec3(
            x: focus.x + sin * SkirmishCamera.pitchCosine * distance,
            y: focus.y + SkirmishCamera.pitchSine * distance,
            z: focus.z + cos * SkirmishCamera.pitchCosine * distance)
        let half = halfAngle(heading)
        let yaw = Quat(x: 0, y: half.sin, z: 0, w: half.cos)
        // Half of 55 degrees, downwards.
        let pitch = Quat(x: -0.461_749, y: 0, z: 0, w: 0.887_011)
        return Pose(position: eye, rotation: product(yaw, pitch))
    }

    /// The ground the camera frames, about, as a rectangle on the map: what the minimap outlines.
    var footprint: (minX: Float, minZ: Float, maxX: Float, maxZ: Float) {
        let half = distance * 0.55
        return (focus.x - half, focus.z - half * 0.75, focus.x + half, focus.z + half * 0.75)
    }

    // --- Arithmetic ------------------------------------------------------------------------------

    private mutating func move(by step: Vec2) {
        guard step.x != 0 || step.y != 0 else { return }
        focus = SkirmishCamera.clamped(Vec3(x: focus.x + step.x, y: focus.y, z: focus.z + step.y))
    }

    private func edge(_ position: Float, origin: Float, extent: Float) -> Float {
        if position < origin + SkirmishCamera.edgeBand { return -1 }
        if position > origin + extent - SkirmishCamera.edgeBand { return 1 }
        return 0
    }

    private static func clamped(_ point: Vec3) -> Vec3 {
        Vec3(
            x: min(max(point.x, bounds.minX), bounds.maxX), y: 0,
            z: min(max(point.z, bounds.minZ), bounds.maxZ))
    }

    private func halfAngle(_ angle: Angle) -> (sin: Float, cos: Float) {
        let half = Angle(raw: angle.raw >> 1)
        return (Float(Detmath.sin(half).presentation), Float(Detmath.cos(half).presentation))
    }

    private func product(_ a: Quat, _ b: Quat) -> Quat {
        Quat(
            x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z)
    }
}
