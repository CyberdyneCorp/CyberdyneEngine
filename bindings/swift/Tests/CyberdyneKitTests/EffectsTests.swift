// SPDX-License-Identifier: MIT
// EffectsTests.swift — the `Effect`, `DecalMaterial` and `Decal` facades through `FakeEngine`.
// ABI 1.15.
//
// Each case checks that a facade call reaches its entry with the right arguments — the parameters,
// the decal's frame and the struct size — converts the answer, and turns a refusal into
// `CyberdyneError.status`.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries saw.
private enum Seen {
    nonisolated(unsafe) static var played = CyEffectPlay()
    nonisolated(unsafe) static var playedEffect: CyEffect = 0
    nonisolated(unsafe) static var spawned = CyDecalDesc()
}

private let kSparks: CyEffect = 4
private let kScorch: CyDecalMaterial = 9

final class EffectsTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Seen.played = CyEffectPlay()
        Seen.playedEffect = 0
        Seen.spawned = CyDecalDesc()
        FakeEngine.install { table in
            table.effect_find = { _, name, out in
                guard let name, String(cString: name) == "impact.sparks", let out else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no such effect")
                }
                out.pointee = kSparks
                return CY_RESULT_OK
            }
            table.effect_play = { _, effect, play in
                guard let play, play.pointee.parameter_count <= 8 else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "too many parameters")
                }
                Seen.playedEffect = effect
                Seen.played = play.pointee
                return CY_RESULT_OK
            }
            table.decal_find_material = { _, name, out in
                guard let name, String(cString: name) == "impact.scorch", let out else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no such material")
                }
                out.pointee = kScorch
                return CY_RESULT_OK
            }
            table.decal_spawn = { _, desc, out in
                guard let desc, let out else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "no description")
                }
                Seen.spawned = desc.pointee
                // A mark of no importance into a full budget is the one evicted.
                out.pointee = desc.pointee.importance == 0 ? 0 : 77
                return CY_RESULT_OK
            }
            table.decal_status = { _, out in
                guard let out, out.pointee.struct_size == UInt32(MemoryLayout<CyDecalStatus>.size)
                else { return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "a struct size") }
                out.pointee.live = 5
                out.pointee.capacity = 64
                out.pointee.spawned = 70
                out.pointee.evicted = 6
                return CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testAnEffectResolvesByNameAndPlaysWithItsParameters() throws {
        let sparks = try Effect.named("impact.sparks")
        XCTAssertEqual(sparks.handle, kSparks)
        try sparks.play(at: Vec3(x: 1, y: 2, z: 3), scale: 0.5, parameters: [0, 1, 0, 0, 7])
        XCTAssertEqual(Seen.playedEffect, kSparks)
        XCTAssertEqual(Seen.played.struct_size, UInt32(MemoryLayout<CyEffectPlay>.size))
        XCTAssertEqual(Seen.played.position.2, 3)
        XCTAssertEqual(Seen.played.scale, 0.5)
        XCTAssertEqual(Seen.played.parameter_count, 5)
        XCTAssertEqual(Seen.played.parameters.1, 1)
        XCTAssertEqual(Seen.played.parameters.4, 7)
        XCTAssertEqual(Seen.played.parameters.5, 0)
        XCTAssertEqual(Seen.played.reserved.0, 0)
    }

    func testAnUnknownNameAndTooManyParametersThrowTheirStatus() {
        XCTAssertThrowsError(try Effect.named("muzzle.smoke")) { error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("\(error)")
            }
            XCTAssertEqual(status, .notFound)
        }
        XCTAssertThrowsError(
            try Effect(handle: kSparks).play(
                at: Vec3(), parameters: [Float](repeating: 1, count: 9))
        ) { error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("\(error)")
            }
            XCTAssertEqual(status, .invalidArgument)
        }
    }

    func testAMarkFacesTheSurfaceItIsSpawnedOn() throws {
        let scorch = try DecalMaterial.named("impact.scorch")
        XCTAssertEqual(scorch.handle, kScorch)
        let decal = try Decal.spawn(
            .mark(scorch, at: Vec3(x: 2, y: 0, z: 1), normal: Vec3(x: 0, y: 1, z: 0), radius: 0.1))
        XCTAssertEqual(decal?.id, 77)
        XCTAssertEqual(Seen.spawned.struct_size, UInt32(MemoryLayout<CyDecalDesc>.size))
        XCTAssertEqual(Seen.spawned.material, kScorch)
        XCTAssertEqual(Seen.spawned.half_extent.0, 0.1)
        XCTAssertEqual(Seen.spawned.half_extent.2, 0.05)
        // +Z turned onto +Y: a quarter turn about -X.
        let rotation = Seen.spawned.pose.rotation
        XCTAssertEqual(rotation.0, -0.5.squareRoot(), accuracy: 1e-6)
        XCTAssertEqual(rotation.1, 0, accuracy: 1e-6)
        XCTAssertEqual(rotation.2, 0, accuracy: 1e-6)
        XCTAssertEqual(rotation.3, 0.5.squareRoot(), accuracy: 1e-6)
    }

    func testTheTurnOntoANormalTakesPlusZThere() {
        let normals = [
            Vec3(x: 0, y: 0, z: 1), Vec3(x: 0, y: 0, z: -1), Vec3(x: 1, y: 0, z: 0),
            Vec3(x: 0, y: -1, z: 0), Vec3(x: 0.6, y: 0, z: 0.8),
        ]
        for normal in normals {
            let q = turning(onto: normal)
            // Rotate +Z by q: v' = v + 2w(u × v) + 2u × (u × v), u = (x, y, z).
            let u = Vec3(x: q.x, y: q.y, z: q.z)
            let uv = Vec3(x: u.y, y: -u.x, z: 0)  // u × (0, 0, 1)
            let uuv = Vec3(
                x: u.y * uv.z - u.z * uv.y, y: u.z * uv.x - u.x * uv.z, z: u.x * uv.y - u.y * uv.x)
            let turned = Vec3(
                x: 2 * q.w * uv.x + 2 * uuv.x, y: 2 * q.w * uv.y + 2 * uuv.y,
                z: 1 + 2 * q.w * uv.z + 2 * uuv.z)
            XCTAssertEqual(turned.x, normal.x, accuracy: 1e-5)
            XCTAssertEqual(turned.y, normal.y, accuracy: 1e-5)
            XCTAssertEqual(turned.z, normal.z, accuracy: 1e-5)
        }
    }

    func testAMarkTheBudgetEvictedIsNilAndTheStatusReads() throws {
        let scorch = DecalMaterial(handle: kScorch)
        let evicted = try Decal.spawn(
            .mark(scorch, at: Vec3(), normal: Vec3(x: 0, y: 0, z: 1), radius: 0.1, importance: 0))
        XCTAssertNil(evicted)
        let status = try Decal.status
        XCTAssertEqual(status.live, 5)
        XCTAssertEqual(status.capacity, 64)
        XCTAssertEqual(status.spawned, 70)
        XCTAssertEqual(status.evicted, 6)
    }
}
