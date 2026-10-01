// SPDX-License-Identifier: MIT
// BodiesTests.swift — `RigidBody` and `CharacterController` through `FakeEngine`.
// `add-swift-m12-gaps`.
//
// The facades' half: the right entity and vectors reach the entries, a nil half crosses as a null
// pointer, the description's nil fields cross as zero (the engine's "default"), the state is
// converted field by field, and a refusal is a thrown `CyberdyneError`. The engine's half — motion
// types, the step guard, the controller itself — is integration.game_backend_bodies and
// integration.game_backend_character.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

private enum Sent {
    nonisolated(unsafe) static var entity: CyEntity = 0
    nonisolated(unsafe) static var vector: [Float] = []
    nonisolated(unsafe) static var point: [Float]? = nil
    nonisolated(unsafe) static var linear: [Float]? = nil
    nonisolated(unsafe) static var angular: [Float]? = nil
    nonisolated(unsafe) static var desc = CyCharacterDesc()
    nonisolated(unsafe) static var input = CyCharacterInput()

    static func three(_ pointer: UnsafePointer<Float>?) -> [Float]? {
        pointer.map { [$0[0], $0[1], $0[2]] }
    }

    static func reset() {
        entity = 0
        vector = []
        point = nil
        linear = nil
        angular = nil
        desc = CyCharacterDesc()
        input = CyCharacterInput()
    }
}

final class BodiesTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Sent.reset()
        FakeEngine.install { table in
            table.physics_apply_force = { _, entity, force in
                Sent.entity = entity
                Sent.vector = Sent.three(force) ?? []
                return CY_RESULT_OK
            }
            table.physics_apply_impulse = { _, entity, impulse, point in
                Sent.entity = entity
                Sent.vector = Sent.three(impulse) ?? []
                Sent.point = Sent.three(point)
                return CY_RESULT_OK
            }
            table.physics_apply_torque = { _, entity, _ in
                Sent.entity = entity
                return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "a static body")
            }
            table.physics_set_velocity = { _, entity, linear, angular in
                Sent.entity = entity
                Sent.linear = Sent.three(linear)
                Sent.angular = Sent.three(angular)
                return CY_RESULT_OK
            }
            table.physics_get_velocity = { _, _, linear, angular in
                linear?[0] = 1
                linear?[1] = 2
                linear?[2] = 3
                angular?[1] = 4
                return CY_RESULT_OK
            }
            table.character_create = { _, entity, desc in
                Sent.entity = entity
                Sent.desc = desc!.pointee
                return CY_RESULT_OK
            }
            table.character_move = { _, entity, input in
                Sent.entity = entity
                Sent.input = input!.pointee
                return CY_RESULT_OK
            }
            table.character_state = { _, _, out in
                guard out!.pointee.struct_size == UInt32(MemoryLayout<CyCharacterState>.size) else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "struct_size")
                }
                out!.pointee.ground = GroundState.grounded.rawValue
                out!.pointee.flags = CY_CHARACTER_STEPPED_UP | CY_CHARACTER_TOUCHING_WALL
                out!.pointee.ground_entity = 12
                out!.pointee.position = (1, 2, 3)
                out!.pointee.platform_velocity = (0, 0, 4)
                return CY_RESULT_OK
            }
            table.character_destroy = { _, _ in
                FakeEngine.fail(CY_RESULT_NOT_FOUND, "the entity has no character")
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testRigidBodySendsTheEntityAndTheVectors() throws {
        let body = RigidBody(Entity(bits: 9))
        try body.applyForce(Vec3(x: 1, y: 2, z: 3))
        XCTAssertEqual(Sent.entity, 9)
        XCTAssertEqual(Sent.vector, [1, 2, 3])

        try body.applyImpulse(Vec3(x: 0, y: 5, z: 0))
        XCTAssertNil(Sent.point, "no point is the centre of mass: a null pointer")
        try body.applyImpulse(Vec3(x: 0, y: 5, z: 0), at: Vec3(x: 1, y: 1, z: 1))
        XCTAssertEqual(Sent.point, [1, 1, 1])

        try body.setVelocity(angular: Vec3(x: 0, y: 2, z: 0))
        XCTAssertNil(Sent.linear, "a nil half is kept: a null pointer")
        XCTAssertEqual(Sent.angular, [0, 2, 0])

        let velocity = try body.velocity
        XCTAssertEqual(velocity.linear, Vec3(x: 1, y: 2, z: 3))
        XCTAssertEqual(velocity.angular, Vec3(x: 0, y: 4, z: 0))
    }

    func testARefusedBodyWriteThrowsTheEnginesStatus() {
        XCTAssertThrowsError(try RigidBody(Entity(bits: 9)).applyTorque(Vec3(x: 0, y: 1, z: 0))) {
            XCTAssertEqual(
                $0 as? CyberdyneError, .status(.invalidArgument, message: "a static body"))
        }
        FakeEngine.uninstall()
        XCTAssertThrowsError(try RigidBody(Entity(bits: 9)).applyForce(Vec3())) { error in
            guard case CyberdyneError.status(.unavailable, _) = error else {
                return XCTFail("expected unavailable, got \(error)")
            }
        }
    }

    func testACharacterDescriptionSendsZeroForEveryDefault() throws {
        let hero = try CharacterController.create(
            on: Entity(bits: 4),
            CharacterController.Description(radius: 0.4, pushesBodies: false, layer: 2, mask: 1))
        XCTAssertEqual(hero.entity, Entity(bits: 4))
        XCTAssertEqual(Sent.entity, 4)
        XCTAssertEqual(Sent.desc.struct_size, UInt32(MemoryLayout<CyCharacterDesc>.size))
        XCTAssertEqual(Sent.desc.radius, 0.4)
        XCTAssertEqual(Sent.desc.height, 0, "nil is the engine's default, which is zero")
        XCTAssertEqual(Sent.desc.step_offset, 0)
        XCTAssertEqual(Sent.desc.flags, CY_CHARACTER_NO_PUSH)
        XCTAssertEqual(Sent.desc.layer, 2)
        XCTAssertEqual(Sent.desc.mask, 1)
    }

    func testAMoveSendsTheVelocityAndAJumpOnlyWhenAsked() throws {
        let hero = CharacterController(Entity(bits: 4))
        try hero.move(velocity: Vec3(x: 3, y: 0, z: 0))
        XCTAssertEqual(Sent.input.desired_velocity.0, 3)
        XCTAssertEqual(Sent.input.flags, 0)
        try hero.move(velocity: Vec3(), jump: 5)
        XCTAssertEqual(Sent.input.flags, CY_CHARACTER_JUMP)
        XCTAssertEqual(Sent.input.jump_speed, 5)
        XCTAssertEqual(Sent.input.struct_size, UInt32(MemoryLayout<CyCharacterInput>.size))
    }

    func testTheCharacterStateIsConvertedFieldByField() throws {
        let state = try CharacterController(Entity(bits: 4)).state
        XCTAssertTrue(state.isGrounded)
        XCTAssertEqual(state.groundEntity, Entity(bits: 12))
        XCTAssertEqual(state.position, Vec3(x: 1, y: 2, z: 3))
        XCTAssertEqual(state.platformVelocity, Vec3(x: 0, y: 0, z: 4))
        XCTAssertTrue(state.steppedUp)
        XCTAssertTrue(state.touchingWall)
        XCTAssertFalse(state.touchingCeiling)
        XCTAssertThrowsError(try CharacterController(Entity(bits: 4)).destroy()) {
            XCTAssertEqual(
                $0 as? CyberdyneError, .status(.notFound, message: "the entity has no character"))
        }
    }
}
