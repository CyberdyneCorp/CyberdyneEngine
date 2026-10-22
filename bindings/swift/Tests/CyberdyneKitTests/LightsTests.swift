// SPDX-License-Identifier: MIT
// LightsTests.swift — the `Light` facade through `FakeEngine`. ABI 1.11.
//
// Each case checks that a facade call reaches its entry with the right arguments — the kind, the
// size, the two-sided flag and the struct size — converts the answer, and turns a refusal into
// `CyberdyneError.status`.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries saw.
private enum Seen {
    nonisolated(unsafe) static var spawned = CyLightDesc()
    nonisolated(unsafe) static var updated = CyLightDesc()
    nonisolated(unsafe) static var updatedLight: CyLight = 0
    nonisolated(unsafe) static var destroyed: CyLight = 0
}

private let kLive: CyLight = 0x2_0000_0007
private let kEnded: CyLight = 0x1_0000_0007

final class LightsTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Seen.spawned = CyLightDesc()
        Seen.updated = CyLightDesc()
        Seen.updatedLight = 0
        Seen.destroyed = 0
        FakeEngine.install { table in
            table.light_spawn = { _, desc, out in
                guard let desc, let out else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "no description")
                }
                guard desc.pointee.intensity >= 0 else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "a negative intensity")
                }
                guard desc.pointee.intensity < 1e9 else {
                    return FakeEngine.fail(CY_RESULT_OUT_OF_RANGE, "the pool is full")
                }
                Seen.spawned = desc.pointee
                out.pointee = kLive
                return CY_RESULT_OK
            }
            table.light_update = { _, light, desc in
                guard light == kLive, let desc else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "the light ended")
                }
                Seen.updatedLight = light
                Seen.updated = desc.pointee
                return CY_RESULT_OK
            }
            table.light_destroy = { _, light in
                guard light == kLive else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "the light ended")
                }
                Seen.destroyed = light
                return CY_RESULT_OK
            }
            table.light_alive = { _, light, out in
                out?.pointee = light == kLive ? 1 : 0
                return CY_RESULT_OK
            }
            table.light_status = { _, out in
                guard let out, out.pointee.struct_size == UInt32(MemoryLayout<CyLightStatus>.size)
                else { return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "a struct size") }
                out.pointee.live = 3
                out.pointee.capacity = 256
                out.pointee.spawned = 40
                out.pointee.expired = 37
                out.pointee.refused = 1
                return CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testAPanelCrossesWithItsKindSizeAndFaces() throws {
        let pose = Pose(position: Vec3(x: 1, y: 2.5, z: -3))
        let light = try Light.spawn(
            .rect(
                pose: pose, width: 1.2, height: 0.4, colour: Vec3(x: 0.4, y: 0.8, z: 1),
                nits: 4000, twoSided: true, lifetime: 2))
        XCTAssertEqual(light.handle, kLive)
        XCTAssertEqual(Seen.spawned.struct_size, UInt32(MemoryLayout<CyLightDesc>.size))
        XCTAssertEqual(Seen.spawned.kind, CY_LIGHT_RECT.rawValue)
        XCTAssertEqual(Seen.spawned.pose.position.1, 2.5)
        XCTAssertEqual(Seen.spawned.size.0, 1.2)
        XCTAssertEqual(Seen.spawned.size.1, 0.4)
        XCTAssertEqual(Seen.spawned.color.1, 0.8)
        XCTAssertEqual(Seen.spawned.intensity, 4000)
        XCTAssertEqual(Seen.spawned.flags, CY_LIGHT_TWO_SIDED)
        XCTAssertEqual(Seen.spawned.lifetime, 2)
        XCTAssertEqual(Seen.spawned.reserved.0, 0)
        // And back: every field the description carries reads as it was written.
        let read = Light.Description(Seen.spawned)
        XCTAssertEqual(read.raw.kind, Seen.spawned.kind)
        XCTAssertEqual(read.kind, .rect)
        XCTAssertTrue(read.twoSided)
        XCTAssertEqual(read.width, 1.2)
        XCTAssertEqual(read.pose, pose)
    }

    func testEveryKindMapsToItsAbiSpelling() {
        let kinds: [(Light.Description, CyLightKind)] = [
            (.point(at: Vec3(), candela: 10, range: 2), CY_LIGHT_POINT),
            (
                .spot(pose: Pose(), candela: 10, range: 2, innerCone: 0.2, outerCone: 0.4),
                CY_LIGHT_SPOT
            ),
            (.rect(pose: Pose(), width: 1, height: 1, nits: 10), CY_LIGHT_RECT),
            (.disc(pose: Pose(), diameter: 1, nits: 10), CY_LIGHT_DISC),
            (.sphere(at: Vec3(), diameter: 1, nits: 10), CY_LIGHT_SPHERE),
            (.tube(pose: Pose(), length: 1, diameter: 0.05, nits: 10), CY_LIGHT_TUBE),
        ]
        for (description, kind) in kinds {
            XCTAssertEqual(description.raw.kind, kind.rawValue)
            XCTAssertEqual(Light.Description(description.raw), description)
        }
        // A tube's length is its width, its diameter its height.
        let tube = Light.Description.tube(pose: Pose(), length: 1.5, diameter: 0.04, nits: 10)
        XCTAssertEqual(tube.raw.size.0, 1.5)
        XCTAssertEqual(tube.raw.size.1, 0.04)
    }

    func testMovingKeepsTheRestOfTheDescription() throws {
        let torch = Light.Description.point(at: Vec3(), candela: 300, range: 8)
        let light = try Light.spawn(torch)
        try light.move(torch, to: Pose(position: Vec3(x: 4, y: 1, z: 0)))
        XCTAssertEqual(Seen.updatedLight, kLive)
        XCTAssertEqual(Seen.updated.pose.position.0, 4)
        XCTAssertEqual(Seen.updated.intensity, 300)
        XCTAssertEqual(Seen.updated.range, 8)
    }

    func testAnEndedLightIsNotAliveAndRefusesWithNotFound() throws {
        XCTAssertTrue(try Light(handle: kLive).isAlive)
        XCTAssertFalse(try Light(handle: kEnded).isAlive)
        XCTAssertThrowsError(try Light(handle: kEnded).destroy()) { error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("\(error)")
            }
            XCTAssertEqual(status, .notFound)
        }
        try Light(handle: kLive).destroy()
        XCTAssertEqual(Seen.destroyed, kLive)
    }

    func testAFullPoolAndANegativeIntensityThrowTheirStatus() {
        XCTAssertThrowsError(try Light.spawn(.point(at: Vec3(), candela: 2e9, range: 1))) {
            error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("\(error)")
            }
            XCTAssertEqual(status, .outOfRange)
        }
        XCTAssertThrowsError(try Light.spawn(.point(at: Vec3(), candela: -1, range: 1))) {
            error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("\(error)")
            }
            XCTAssertEqual(status, .invalidArgument)
        }
    }

    func testThePoolStatusReads() throws {
        let status = try Light.status
        XCTAssertEqual(status.live, 3)
        XCTAssertEqual(status.capacity, 256)
        XCTAssertEqual(status.spawned, 40)
        XCTAssertEqual(status.expired, 37)
        XCTAssertEqual(status.refused, 1)
    }
}
