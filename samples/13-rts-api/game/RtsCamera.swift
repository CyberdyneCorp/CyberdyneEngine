// SPDX-License-Identifier: MIT
// RtsCamera.swift — an RTS camera: a point on the ground that the keys and the screen edges move.
//
// The host's camera rig follows a target from a fixed height and angle and looks at it. This type
// decides where the target is. It reads the keys (`camera.pan`) and the pointer, and moves the
// focus with `Camera.setTarget`. Everything here runs in `onUpdate`: the pointer is device state,
// so the engine refuses to hand it to a fixed step.

import CyberdyneKit

struct RtsCamera {
    /// The ground point the camera frames.
    var focus: Vec3
    /// Metres per second at full deflection.
    var speed: Float
    /// How close to a screen edge, in pixels, the pointer has to be to pan.
    var edgeBand: Float
    /// Whether the focus has been handed to the camera at least once.
    private var placed = false

    init(focus: Vec3, speed: Float, edgeBand: Float) {
        self.focus = focus
        self.speed = speed
        self.edgeBand = edgeBand
    }

    /// Where to pan this frame, as world x and z, each clamped to -1...1. The keys' up axis is
    /// world -z, away from a camera that looks down the -z axis.
    func direction(keys: Vec2, pointer: Pointer, viewport: Vec4) -> Vec2 {
        var x = keys.x
        var z = -keys.y
        if pointer.isPresent && pointer.isInWindow && !pointer.isOverUI {
            x += edge(pointer.position.x, origin: viewport.x, extent: viewport.z)
            z += edge(pointer.position.y, origin: viewport.y, extent: viewport.w)
        }
        return Vec2(x: clamped(x), y: clamped(z))
    }

    /// Move the focus by `direction` over `delta` seconds and tell the camera. It does nothing when
    /// the camera already frames the focus and there is nothing to move.
    mutating func pan(_ camera: Camera, direction: Vec2, delta: Float) throws {
        let moving = direction.x != 0 || direction.y != 0
        guard moving || !placed else { return }
        focus.x += direction.x * speed * delta
        focus.z += direction.y * speed * delta
        try camera.setTarget(CameraTarget(focus: .position(focus)))
        placed = true
    }

    private func edge(_ position: Float, origin: Float, extent: Float) -> Float {
        if position < origin + edgeBand { return -1 }
        if position > origin + extent - edgeBand { return 1 }
        return 0
    }

    private func clamped(_ value: Float) -> Float {
        min(max(value, -1), 1)
    }
}
