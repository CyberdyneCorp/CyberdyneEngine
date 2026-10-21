// SPDX-License-Identifier: MIT
// FogOfWarTests.swift — the `FogOfWar` facade through `FakeEngine`. ABI 1.10.
//
// Each case checks that a facade call reaches its entry with the right arguments — the reveal
// radius and the position as raw `Fixed` bits, never through a float — converts the answer, and
// turns a refusal into `CyberdyneError.status`.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries saw.
private enum Seen {
    nonisolated(unsafe) static var vision = CyFogVision()
    nonisolated(unsafe) static var visionOf: CyEntity = 0
    nonisolated(unsafe) static var cell: (UInt32, CyFixedVec2) = (0, CyFixedVec2())
    nonisolated(unsafe) static var sight: (UInt32, CyEntity) = (0, 0)
}

private let kScout: CyEntity = 0x1_0000_0021
private let kKeep: CyEntity = 0x1_0000_0022
private let kStranger: CyEntity = 0x9_0000_0001

final class FogOfWarTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Seen.vision = CyFogVision()
        Seen.visionOf = 0
        Seen.cell = (0, CyFixedVec2())
        Seen.sight = (0, 0)
        FakeEngine.install { table in
            table.fog_set_vision = { _, entity, vision in
                guard let vision else { return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "no") }
                guard entity != kStranger else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "the host places no such entity")
                }
                guard vision.pointee.team <= 32, vision.pointee.reveal_radius >= 0 else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "a team or radius")
                }
                Seen.vision = vision.pointee
                Seen.visionOf = entity
                return CY_RESULT_OK
            }
            table.fog_get_vision = { _, entity, out in
                guard entity == Seen.visionOf, let out else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no vision")
                }
                out.pointee = Seen.vision
                return CY_RESULT_OK
            }
            table.fog_cell = { _, team, position, out in
                guard let position, let out else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "no position")
                }
                Seen.cell = (team, position.pointee)
                // Visible west of x = 10 m, explored to 20 m, unexplored beyond.
                let x = position.pointee.x >> 32
                out.pointee =
                    x < 10
                    ? CY_FOG_CELL_VISIBLE.rawValue
                    : (x < 20 ? CY_FOG_CELL_EXPLORED.rawValue : CY_FOG_CELL_UNEXPLORED.rawValue)
                return CY_RESULT_OK
            }
            table.fog_sight = { _, team, entity, out in
                Seen.sight = (team, entity)
                out?.pointee =
                    entity == kKeep
                    ? CY_FOG_SIGHT_GHOST.rawValue
                    : (entity == kScout
                        ? CY_FOG_SIGHT_VISIBLE.rawValue : CY_FOG_SIGHT_HIDDEN.rawValue)
                return CY_RESULT_OK
            }
            table.fog_status = { _, out in
                out?.pointee.width = 64
                out?.pointee.height = 48
                out?.pointee.units = 12
                out?.pointee.tick = 90
                out?.pointee.visible_cells = 311
                out?.pointee.ghosts = 2
                out?.pointee.digest = 0xDEAD_BEEF
                return CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testAVisionCrossesAsRawFixedBitsAndReadsBack() throws {
        let radius = Fixed(raw: (7 << 32) + 3)  // seven metres and three raw steps
        try FogOfWar.setVision(
            .init(team: 2, revealRadius: radius, leavesGhost: true), of: Entity(bits: kScout))
        XCTAssertEqual(Seen.visionOf, kScout)
        XCTAssertEqual(Seen.vision.struct_size, UInt32(MemoryLayout<CyFogVision>.size))
        XCTAssertEqual(Seen.vision.team, 2)
        XCTAssertEqual(Seen.vision.reveal_radius, (7 << 32) + 3)
        XCTAssertEqual(Seen.vision.flags, CY_FOG_LEAVES_GHOST)
        XCTAssertEqual(Seen.vision.reserved, 0)
        let read = try FogOfWar.vision(of: Entity(bits: kScout))
        XCTAssertEqual(read, .init(team: 2, revealRadius: radius, leavesGhost: true))
        XCTAssertEqual(try FogOfWar.revealRadius(of: Entity(bits: kScout)), radius)
    }

    func testRefusalsThrowWithTheirStatus() {
        XCTAssertThrowsError(
            try FogOfWar.setVision(
                .init(team: 33, revealRadius: Fixed(1)), of: Entity(bits: kScout))
        ) { error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("\(error)")
            }
            XCTAssertEqual(status, .invalidArgument)
        }
        XCTAssertThrowsError(
            try FogOfWar.setVision(
                .init(team: 1, revealRadius: Fixed(1)), of: Entity(bits: kStranger))
        ) { error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("\(error)")
            }
            XCTAssertEqual(status, .notFound)
        }
        XCTAssertThrowsError(try FogOfWar.vision(of: Entity(bits: kStranger)))
    }

    func testCellsAndSightConvertTheirAnswers() throws {
        let near = FixedVec2(x: Fixed(4), y: Fixed(-6))
        XCTAssertEqual(try FogOfWar.cell(team: 1, at: near), .visible)
        XCTAssertEqual(Seen.cell.0, 1)
        XCTAssertEqual(Seen.cell.1.x, 4 << 32)
        XCTAssertEqual(Seen.cell.1.y, -(6 << 32))
        XCTAssertTrue(try FogOfWar.isVisible(team: 1, at: near))
        XCTAssertEqual(try FogOfWar.cell(team: 3, at: FixedVec2(x: Fixed(15))), .explored)
        XCTAssertEqual(try FogOfWar.cell(team: 3, at: FixedVec2(x: Fixed(25))), .unexplored)
        XCTAssertFalse(try FogOfWar.isVisible(team: 3, at: FixedVec2(x: Fixed(15))))
        XCTAssertEqual(try FogOfWar.sight(team: 2, of: Entity(bits: kScout)), .visible)
        XCTAssertEqual(try FogOfWar.sight(team: 2, of: Entity(bits: kKeep)), .ghost)
        XCTAssertEqual(try FogOfWar.sight(team: 2, of: Entity(bits: kStranger)), .hidden)
        XCTAssertEqual(Seen.sight.0, 2)
        XCTAssertEqual(Seen.sight.1, kStranger)
    }

    func testStatusReadsEveryField() throws {
        let status = try FogOfWar.status
        XCTAssertEqual(status.width, 64)
        XCTAssertEqual(status.height, 48)
        XCTAssertEqual(status.units, 12)
        XCTAssertEqual(status.tick, 90)
        XCTAssertEqual(status.visibleCells, 311)
        XCTAssertEqual(status.ghosts, 2)
        XCTAssertEqual(status.digest, 0xDEAD_BEEF)
    }
}
