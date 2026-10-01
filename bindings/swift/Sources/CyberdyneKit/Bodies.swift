// SPDX-License-Identifier: MIT
// Bodies.swift — ABI 1.5's rigid-body writes, as a game writes them. `add-swift-m12-gaps`.
//
//     let crate = RigidBody(entity)
//     try crate.applyImpulse(Vec3(x: 0, y: 6, z: 0))              // at once
//     try crate.applyForce(Vec3(x: 40, y: 0, z: 0))               // over the next step
//     try crate.setVelocity(linear: .zero)                        // the angular half is kept
//     let v = try crate.velocity.linear                           // any phase
//
// PHASES. The writes are simulation: allowed in `onFixedUpdate` and at initialisation, refused in
// `onUpdate` with `CyberdyneError.status(.permissionDenied, …)`. Reading the velocity is allowed
// everywhere. Every call throws `.unavailable` while the physics step itself runs.
//
// WHICH BODY. The one the entity owns, through the embedder's entity-to-body map. An entity with no
// body throws `.notFound`; a static body, or a kinematic one under a force or an impulse, throws
// `.invalidArgument` — the server would ignore the push, and a push that silently does nothing is
// a bug the game should hear about.
//
// DETERMINISM. Forces and torques accumulate until the next step; impulses and velocities apply at
// once. Within one fixed step they take effect in call order, which is script order.

import CyberdyneABI
import CyberdyneCore

/// The physics body an entity owns.
public struct RigidBody: Hashable, Sendable {
    /// The entity whose body this is.
    public let entity: Entity

    /// The body `entity` owns. Nothing is checked until a call is made.
    public init(_ entity: Entity) {
        self.entity = entity
    }

    /// Add `force` (newtons, world space) at the centre of mass for the next step. Wakes the body.
    public func applyForce(_ force: Vec3) throws {
        let engine = try GameServices.engine()
        let value = [force.x, force.y, force.z]
        try value.withUnsafeBufferPointer {
            try engine.physicsApplyForce(entity: entity.bits, force: $0.baseAddress)
        }
    }

    /// Apply `impulse` (newton seconds, world space) now — at the world-space `point`, or at the
    /// centre of mass when it is nil.
    public func applyImpulse(_ impulse: Vec3, at point: Vec3? = nil) throws {
        let engine = try GameServices.engine()
        let value = [impulse.x, impulse.y, impulse.z]
        let pointValues = point.map { [$0.x, $0.y, $0.z] }
        try value.withUnsafeBufferPointer { impulse in
            try withOptionalBuffer(pointValues) { point in
                try engine.physicsApplyImpulse(
                    entity: entity.bits, impulse: impulse.baseAddress, point: point)
            }
        }
    }

    /// Add `torque` (newton metres, world space) for the next step. Wakes the body.
    public func applyTorque(_ torque: Vec3) throws {
        let engine = try GameServices.engine()
        let value = [torque.x, torque.y, torque.z]
        try value.withUnsafeBufferPointer {
            try engine.physicsApplyTorque(entity: entity.bits, torque: $0.baseAddress)
        }
    }

    /// Replace the velocity. A nil half is kept as it is.
    public func setVelocity(linear: Vec3? = nil, angular: Vec3? = nil) throws {
        let engine = try GameServices.engine()
        try withOptionalBuffer(linear.map { [$0.x, $0.y, $0.z] }) { linear in
            try withOptionalBuffer(angular.map { [$0.x, $0.y, $0.z] }) { angular in
                try engine.physicsSetVelocity(entity: entity.bits, linear: linear, angular: angular)
            }
        }
    }

    /// The linear and angular velocity after the last step and any write since.
    public var velocity: (linear: Vec3, angular: Vec3) {
        get throws {
            let engine = try GameServices.engine()
            var linear = [Float](repeating: 0, count: 3)
            var angular = [Float](repeating: 0, count: 3)
            try linear.withUnsafeMutableBufferPointer { linear in
                try angular.withUnsafeMutableBufferPointer { angular in
                    try engine.physicsGetVelocity(
                        entity: entity.bits, linear: linear.baseAddress,
                        angular: angular.baseAddress)
                }
            }
            return (
                Vec3(x: linear[0], y: linear[1], z: linear[2]),
                Vec3(x: angular[0], y: angular[1], z: angular[2])
            )
        }
    }
}

/// Call `body` with a pointer to `values`, or with nil when there are none — the ABI's "null keeps
/// that half".
func withOptionalBuffer<Result>(
    _ values: [Float]?, _ body: (UnsafePointer<Float>?) throws -> Result
) rethrows -> Result {
    guard let values else { return try body(nil) }
    return try values.withUnsafeBufferPointer { try body($0.baseAddress) }
}
