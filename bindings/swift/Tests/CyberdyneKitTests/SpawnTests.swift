// SPDX-License-Identifier: MIT
// SpawnTests.swift — the `Spawn` facade and `World.spawn` through `FakeEngine`.
// `add-swift-game-api`.
//
// OWNER: implementer C. Each case checks that a facade call reaches its entry with the right
// arguments, converts the answer, and turns a refusal into `CyberdyneError.status`.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries saw.
private enum Seen {
    nonisolated(unsafe) static var params = CySpawnParams()
    nonisolated(unsafe) static var prefab: CyPrefab = 0
    nonisolated(unsafe) static var parent: CyEntity = 0
    nonisolated(unsafe) static var poses: [CyPose] = []
    nonisolated(unsafe) static var destroyed: CyEntity = 0
}

private let kTank: CyPrefab = 0x1_0000_0000

final class SpawnTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Seen.params = CySpawnParams()
        Seen.poses = []
        FakeEngine.install { table in
            table.spawn_resolve = { _, asset, prefab in
                guard let asset, String(cString: asset) == "units/tank" else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "spawn_resolve: no prefab")
                }
                prefab?.pointee = kTank
                return CY_RESULT_OK
            }
            table.spawn_instantiate = { _, prefab, params, root in
                Seen.prefab = prefab
                Seen.params = params?.pointee ?? CySpawnParams()
                root?.pointee = 0x10
                return CY_RESULT_OK
            }
            table.spawn_instantiate_many = { _, prefab, parent, poses, count, roots in
                Seen.prefab = prefab
                Seen.parent = parent
                Seen.poses = Array(UnsafeBufferPointer(start: poses, count: Int(count)))
                for index in 0..<Int(count) {
                    roots?[index] = CyEntity(0x20 + index)
                }
                return CY_RESULT_OK
            }
            table.spawn_destroy = { _, root in
                guard root == 0x10 else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "spawn_destroy: not alive")
                }
                Seen.destroyed = root
                return CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testWorldSpawnResolvesAndInstantiatesAtThePose() throws {
        let pose = Pose(position: Vec3(x: 4, y: 0, z: -2), rotation: Quat(x: 0, y: 1, z: 0, w: 0))
        let tank = try World.spawn(prefab: "units/tank", at: pose, parent: Entity(bits: 7))
        XCTAssertEqual(tank, Entity(bits: 0x10))
        XCTAssertEqual(Seen.prefab, kTank)
        XCTAssertEqual(Seen.params.struct_size, UInt32(MemoryLayout<CySpawnParams>.size))
        XCTAssertEqual(Seen.params.parent, 7)
        XCTAssertEqual(Pose(Seen.params.pose), pose)
        XCTAssertEqual(Seen.params.scale.0, 1)
        XCTAssertEqual(Seen.params.scale.2, 1)
    }

    func testAPrefabInstantiatesWithAScaleAndNoParent() throws {
        let prefab = try Spawn.prefab("units/tank")
        XCTAssertEqual(prefab, Prefab(raw: kTank))
        try prefab.instantiate(scale: Vec3(x: 2, y: 2, z: 2))
        XCTAssertEqual(Seen.params.parent, CY_ENTITY_NULL)
        XCTAssertEqual(Seen.params.scale.1, 2)
        XCTAssertEqual(Pose(Seen.params.pose), Pose())
    }

    func testABatchSendsEveryPoseInOrderAndReturnsEveryRoot() throws {
        let prefab = try Spawn.prefab("units/tank")
        let poses = (0..<3).map { Pose(position: Vec3(x: Float($0), y: 0, z: 0)) }
        let roots = try prefab.instantiate(at: poses, parent: Entity(bits: 9))
        XCTAssertEqual(roots, [Entity(bits: 0x20), Entity(bits: 0x21), Entity(bits: 0x22)])
        XCTAssertEqual(Seen.parent, 9)
        XCTAssertEqual(Seen.poses.map(Pose.init), poses)
    }

    func testAnEmptyBatchCallsNothing() throws {
        let prefab = try Spawn.prefab("units/tank")
        Seen.prefab = 0
        XCTAssertEqual(try prefab.instantiate(at: []), [])
        XCTAssertEqual(Seen.prefab, 0)
    }

    func testDestroyReachesItsEntryAndARefusalThrows() throws {
        try World.destroy(Entity(bits: 0x10))
        XCTAssertEqual(Seen.destroyed, 0x10)
        XCTAssertThrowsError(try Spawn.destroy(Entity(bits: 0x99))) { error in
            XCTAssertEqual(
                error as? CyberdyneError, .status(.notFound, message: "spawn_destroy: not alive"))
        }
    }

    func testAFrameUpdateRefusalSurfacesAsPermissionDenied() {
        FakeEngine.install { table in
            table.spawn_resolve = { _, _, prefab in
                prefab?.pointee = kTank
                return CY_RESULT_OK
            }
            table.spawn_instantiate = { _, _, _, _ in
                FakeEngine.fail(
                    CY_RESULT_PERMISSION_DENIED,
                    "spawn_instantiate may not be called during frame update")
            }
        }
        XCTAssertThrowsError(try World.spawn(prefab: "units/tank")) { error in
            XCTAssertEqual(
                error as? CyberdyneError,
                .status(
                    .permissionDenied,
                    message: "spawn_instantiate may not be called during frame update"))
        }
    }

    func testAnUnknownPrefabThrowsNotFound() {
        XCTAssertThrowsError(try Spawn.prefab("units/none")) { error in
            XCTAssertEqual(
                error as? CyberdyneError, .status(.notFound, message: "spawn_resolve: no prefab"))
        }
    }
}
