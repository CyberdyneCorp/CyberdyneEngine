// SPDX-License-Identifier: MIT
// GameTypesTests.swift — the shared pieces of the game-service facades, and the harness their
// suites use. `add-swift-game-api`.
//
// Each facade's suite takes these as given: `Pose` and `Ray` cross the boundary unchanged, a facade
// with no engine throws rather than traps, and `FakeEngine` delivers a call and its failure the way
// the engine does.

import XCTest

@testable import CyberdyneKit

final class GameTypesTests: XCTestCase {
    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testAPoseCrossesTheBoundaryUnchanged() {
        let pose = Pose(
            position: Vec3(x: 1, y: 2, z: 3), rotation: Quat(x: 0.1, y: 0.2, z: 0.3, w: 0.9))
        let raw = pose.raw
        XCTAssertEqual(raw.position.0, 1)
        XCTAssertEqual(raw.position.2, 3)
        XCTAssertEqual(raw.rotation.0, 0.1)
        XCTAssertEqual(raw.rotation.3, 0.9)
        XCTAssertEqual(Pose(raw), pose)
    }

    func testTheDefaultPoseIsTheIdentity() {
        XCTAssertEqual(Pose().rotation, Quat(x: 0, y: 0, z: 0, w: 1))
        XCTAssertEqual(Pose().position, Vec3())
    }

    func testARayCrossesTheBoundaryUnchanged() {
        let ray = Ray(
            origin: Vec3(x: 4, y: 5, z: 6), direction: Vec3(x: 0, y: -1, z: 0), maxDistance: 50)
        let raw = ray.raw
        XCTAssertEqual(raw.origin.1, 5)
        XCTAssertEqual(raw.direction.1, -1)
        XCTAssertEqual(raw.max_distance, 50)
        XCTAssertEqual(Ray(raw), ray)
    }

    func testWithNoEngineAFacadeThrowsUnavailable() {
        FakeEngine.uninstall()
        XCTAssertThrowsError(try GameServices.engine()) { error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("expected a status error, got \(error)")
            }
            XCTAssertEqual(status, .unavailable)
        }
    }

    func testTheFakeEngineDeliversACallAndItsAnswer() throws {
        FakeEngine.install { table in
            table.time_get = { _, time in
                time?.pointee.tick = 77
                time?.pointee.phase = Phase.fixedUpdate.rawValue
                return CY_RESULT_OK
            }
        }
        var time = CyTime()
        time.struct_size = UInt32(MemoryLayout<CyTime>.size)
        try GameServices.engine().timeGet(into: &time)
        XCTAssertEqual(time.tick, 77)
        XCTAssertEqual(Phase(rawValue: time.phase), .fixedUpdate)
    }

    func testTheFakeEngineDeliversARefusalAsASwiftError() {
        FakeEngine.install { table in
            table.input_pointer = { _, _, _ in
                FakeEngine.fail(
                    CY_RESULT_PERMISSION_DENIED,
                    "input_pointer may not be called during fixed update")
            }
        }
        var pointer = CyInputPointer()
        XCTAssertThrowsError(try GameServices.engine().inputPointer(user: 0, into: &pointer)) {
            error in
            XCTAssertEqual(
                error as? CyberdyneError,
                .status(
                    .permissionDenied,
                    message: "input_pointer may not be called during fixed update"))
        }
    }
}
