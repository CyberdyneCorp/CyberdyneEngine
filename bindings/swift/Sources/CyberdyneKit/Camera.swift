// SPDX-License-Identifier: MIT
// Camera.swift — ABI 1.3's `camera_*` entries, as a game writes them. `add-swift-game-api`.
//
// OWNER: implementer A (input and camera). See openspec/changes/add-swift-game-api/design.md.
//
//     // onUpdate: pick what is under the cursor
//     let pointer = try Input.pointer()
//     if let camera = try Camera.active(), pointer.pressed.contains(.left) {
//         let ray = try camera.ray(under: pointer)             // feed it to a physics raycast
//     }
//     // health bars over a selection, one call for all of them
//     let marks = try camera.project(selection.map(\.position))
//     // an RTS camera move, allowed in a fixed step too
//     try camera.setTarget(CameraTarget(focus: .position(base), yaw: 0.8, pitch: -0.9, distance: 40))
//
// The camera is presentation. Reading it — `active`, `view`, the projections — is frame-update
// only and throws `.permissionDenied` in a fixed step. Writing it is allowed in a fixed step too,
// because nothing in the simulation reads it back; while a tick is being resimulated the engine
// accepts a write and drops it, so a rollback does not move the camera twice.

import CyberdyneABI
import CyberdyneCore

/// A camera rig, as the engine addresses it. A handle: holding one keeps nothing alive, and a
/// destroyed rig answers `.notFound`.
public struct Camera: Hashable, Sendable {
    /// The engine's rig handle.
    public let raw: CyCamera

    /// Wrap a handle the engine handed out.
    public init(raw: CyCamera) {
        self.raw = raw
    }

    /// The camera of the primary view, or nil when there is none — a dedicated server, a tool with
    /// no viewport. Frame update only.
    public static func active() throws -> Camera? {
        var camera: CyCamera = 0
        do {
            try GameServices.engine().cameraActive(into: &camera)
        } catch CyberdyneError.status(.unavailable, _) {
            return nil
        }
        return Camera(raw: camera)
    }

    /// This camera as last evaluated: pose, projection and viewport. Frame update only.
    public func view() throws -> CameraView {
        var view = CyCameraView()
        view.struct_size = UInt32(MemoryLayout<CyCameraView>.size)
        try GameServices.engine().cameraView(camera: raw, into: &view)
        return CameraView(view)
    }

    /// The world ray under a point in window pixels, from the near plane to the far plane.
    /// Frame update only.
    public func ray(through screen: Vec2) throws -> Ray {
        let engine = try GameServices.engine()
        var ray = CyRay()
        let point = [screen.x, screen.y]
        try point.withUnsafeBufferPointer {
            try engine.cameraScreenToRay(camera: raw, screen: $0.baseAddress, into: &ray)
        }
        return Ray(ray)
    }

    /// The world ray under the pointer. Frame update only.
    public func ray(under pointer: Pointer) throws -> Ray {
        try ray(through: pointer.position)
    }

    /// Where one world point lands on screen. Frame update only.
    public func project(_ point: Vec3) throws -> ScreenPoint {
        try project([point])[0]
    }

    /// Where each world point lands on screen, in the same order, in one engine call.
    /// Frame update only.
    public func project(_ points: [Vec3]) throws -> [ScreenPoint] {
        let engine = try GameServices.engine()
        let coordinates = points.flatMap { [$0.x, $0.y, $0.z] }
        var projected = [CyScreenPoint](repeating: CyScreenPoint(), count: points.count)
        try coordinates.withUnsafeBufferPointer { input in
            try projected.withUnsafeMutableBufferPointer { output in
                try engine.cameraWorldToScreen(
                    camera: raw, points: input.baseAddress, count: UInt32(points.count),
                    into: output.baseAddress)
            }
        }
        return projected.map(ScreenPoint.init)
    }

    /// Point the camera's rig at a focus, from an angle and a distance. Any phase.
    public func setTarget(_ target: CameraTarget) throws {
        var raw = target.raw
        try GameServices.engine().cameraSetTarget(camera: self.raw, target: &raw)
    }

    /// Override the rig with an explicit pose until `clearPoseOverride()`. Any phase.
    public func overridePose(_ pose: Pose) throws {
        var raw = pose.raw
        try GameServices.engine().cameraSetPose(camera: self.raw, pose: &raw)
    }

    /// Hand the camera back to its rig. Any phase; fine when no override was set.
    public func clearPoseOverride() throws {
        try GameServices.engine().cameraClearPose(camera: raw)
    }
}

/// A camera as last evaluated.
public struct CameraView: Equatable, Sendable {
    /// Where the camera is, and where it looks (its local -Z).
    public var pose: Pose
    /// Orthographic rather than perspective.
    public var isOrthographic: Bool
    /// Radians; zero when orthographic.
    public var verticalFieldOfView: Float
    /// Metres; zero when perspective.
    public var orthographicHeight: Float
    /// Metres.
    public var nearPlane: Float
    /// Metres; zero means infinite.
    public var farPlane: Float
    /// x, y, width, height in window pixels.
    public var viewport: Vec4

    /// From the ABI's spelling.
    public init(_ raw: CyCameraView) {
        pose = Pose(raw.pose)
        isOrthographic = raw.flags & CY_CAMERA_VIEW_ORTHOGRAPHIC != 0
        verticalFieldOfView = raw.vertical_fov
        orthographicHeight = raw.ortho_height
        nearPlane = raw.near_plane
        farPlane = raw.far_plane
        viewport = Vec4(
            x: raw.viewport.0, y: raw.viewport.1, z: raw.viewport.2, w: raw.viewport.3)
    }
}

/// Where a world point lands on screen.
public struct ScreenPoint: Equatable, Sendable {
    /// Window pixels, the space `Camera.ray(through:)` takes. Mirrored and unusable when `isBehind`.
    public var position: Vec2
    /// Metres along the camera's forward axis; negative behind it.
    public var depth: Float
    /// Inside the viewport and in front of the near plane.
    public var isOnScreen: Bool
    /// Behind the camera.
    public var isBehind: Bool

    /// From the ABI's spelling.
    public init(_ raw: CyScreenPoint) {
        position = Vec2(raw.position)
        depth = raw.depth
        isOnScreen = raw.flags & CY_SCREEN_POINT_ON_SCREEN != 0
        isBehind = raw.flags & CY_SCREEN_POINT_BEHIND != 0
    }
}

/// What an RTS camera is told: a focus, an angle round it and a distance from it.
public struct CameraTarget: Equatable, Sendable {
    /// What the camera frames.
    public enum Focus: Equatable, Sendable {
        /// A point in world space.
        case position(Vec3)
        /// An entity, followed as it moves.
        case entity(CyEntity)
    }

    /// What the camera frames and orbits.
    public var focus: Focus
    /// Radians about world +Y.
    public var yaw: Float
    /// Radians; negative looks down.
    public var pitch: Float
    /// Metres from the focus.
    public var distance: Float
    /// Seconds to blend there; zero is a cut.
    public var blendSeconds: Float

    /// A target on `focus`; the angles, distance and blend default to zero.
    public init(
        focus: Focus, yaw: Float = 0, pitch: Float = 0, distance: Float = 0,
        blendSeconds: Float = 0
    ) {
        self.focus = focus
        self.yaw = yaw
        self.pitch = pitch
        self.distance = distance
        self.blendSeconds = blendSeconds
    }

    /// In the ABI's spelling.
    public var raw: CyCameraTarget {
        var raw = CyCameraTarget()
        raw.struct_size = UInt32(MemoryLayout<CyCameraTarget>.size)
        switch focus {
        case .position(let point):
            raw.position = point.tuple
        case .entity(let entity):
            raw.flags = CY_CAMERA_TARGET_FOLLOW_ENTITY
            raw.entity = entity
        }
        raw.yaw = yaw
        raw.pitch = pitch
        raw.distance = distance
        raw.blend_seconds = blendSeconds
        return raw
    }
}
