// SPDX-License-Identifier: MIT
// GameTypes.swift — the value types ABI 1.3's game-service facades share. `add-swift-game-api`.
//
// `Input`, `Camera`, `Physics`, `Navigation`, `Audio`, `Spawn` and `Time` are written by three
// implementers in separate files (design.md, "File ownership"). What more than one of them needs —
// a pose, a ray, the tuple conversions C arrays import as, and "the engine, or a thrown error" —
// is declared once, here, so no two facades grow two spellings of the same thing.

import CyberdyneABI
import CyberdyneCore

/// A position and an orientation in world space, as `CyPose` carries them.
///
/// The identity rotation is the default. The ABI reads an all-zero quaternion as the identity too,
/// so a zeroed `CyPose` and `Pose()` mean the same thing.
public struct Pose: Equatable, Sendable {
    /// World-space position, in metres.
    public var position: Vec3
    /// Orientation, as a unit quaternion.
    public var rotation: Quat

    /// A pose at `position` facing `rotation`; the origin and the identity by default.
    public init(position: Vec3 = Vec3(), rotation: Quat = Quat(x: 0, y: 0, z: 0, w: 1)) {
        self.position = position
        self.rotation = rotation
    }

    /// From the ABI's spelling.
    public init(_ raw: CyPose) {
        position = Vec3(raw.position)
        rotation = Quat(
            x: raw.rotation.0, y: raw.rotation.1, z: raw.rotation.2, w: raw.rotation.3)
    }

    /// In the ABI's spelling.
    public var raw: CyPose {
        CyPose(
            position: position.tuple,
            rotation: (rotation.x, rotation.y, rotation.z, rotation.w))
    }
}

/// A ray, as `CyRay` carries it. `direction` is expected to be unit length, so distances are metres.
public struct Ray: Equatable, Sendable {
    /// Where the ray starts, in world space.
    public var origin: Vec3
    /// Which way it points; unit length.
    public var direction: Vec3
    /// How far it reaches, in metres.
    public var maxDistance: Float

    /// A ray from `origin` along `direction`, reaching `maxDistance` metres.
    public init(origin: Vec3, direction: Vec3, maxDistance: Float = 1000) {
        self.origin = origin
        self.direction = direction
        self.maxDistance = maxDistance
    }

    /// From the ABI's spelling.
    public init(_ raw: CyRay) {
        origin = Vec3(raw.origin)
        direction = Vec3(raw.direction)
        maxDistance = raw.max_distance
    }

    /// In the ABI's spelling.
    public var raw: CyRay {
        CyRay(origin: origin.tuple, direction: direction.tuple, max_distance: maxDistance)
    }
}

extension Vec3 {
    /// A C `float[3]`, as Swift imports it.
    public init(_ tuple: (Float, Float, Float)) {
        self.init(x: tuple.0, y: tuple.1, z: tuple.2)
    }

    /// This vector as a C `float[3]`.
    public var tuple: (Float, Float, Float) { (x, y, z) }
}

extension Vec2 {
    /// A C `float[2]`, as Swift imports it.
    public init(_ tuple: (Float, Float)) {
        self.init(x: tuple.0, y: tuple.1)
    }

    /// This vector as a C `float[2]`.
    public var tuple: (Float, Float) { (x, y) }
}

/// Where every game-service facade gets the engine from.
enum GameServices {
    /// The bound engine, or `CyberdyneError.status(.unavailable, …)` before bring-up — a facade
    /// called from a test with no engine, or from a module that failed its version check, throws
    /// rather than traps.
    static func engine() throws -> Engine {
        guard let engine = Runtime.engine else {
            throw CyberdyneError.status(.unavailable, message: "no engine is bound to this module")
        }
        return engine
    }
}
