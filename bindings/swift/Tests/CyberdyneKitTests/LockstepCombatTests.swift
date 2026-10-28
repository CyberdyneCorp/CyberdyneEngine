// SPDX-License-Identifier: MIT
// LockstepCombatTests.swift — `Lockstep.arm`, `fortify`, `combatant`, `structure`, `events` and
// `projectiles`, and the two attack orders, through `FakeEngine`. ABI 1.16.
//
// Each case checks that a facade call reaches its entry with the right raw values — every `Fixed`
// as its raw bits, never through a float — converts the answer, and turns a refusal into
// `CyberdyneError.status`.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries saw.
private enum Seen {
    nonisolated(unsafe) static var armedUnit: UInt32 = 0
    nonisolated(unsafe) static var arms = CyLockstepCombatDesc()
    nonisolated(unsafe) static var fortified = CyLockstepStructureDesc()
    nonisolated(unsafe) static var order = CyLockstepOrder()
    nonisolated(unsafe) static var firstAsked: UInt64 = 0
}

final class LockstepCombatTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Seen.arms = CyLockstepCombatDesc()
        Seen.fortified = CyLockstepStructureDesc()
        Seen.order = CyLockstepOrder()
        FakeEngine.install { table in
            table.lockstep_arm = { _, unit, desc in
                guard let desc, desc.pointee.health > 0 else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "no health")
                }
                Seen.armedUnit = unit
                Seen.arms = desc.pointee
                return CY_RESULT_OK
            }
            table.lockstep_fortify = { _, desc, out in
                guard let desc, let out else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "no desc")
                }
                Seen.fortified = desc.pointee
                out.pointee = 1
                return CY_RESULT_OK
            }
            table.lockstep_order = { _, order in
                guard let order else { return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "") }
                Seen.order = order.pointee
                return CY_RESULT_OK
            }
            table.lockstep_combatant = { _, unit, out in
                guard let out, unit == 3 else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no such unit")
                }
                out.pointee.flags = CY_LOCKSTEP_COMBATANT_ARMED | CY_LOCKSTEP_COMBATANT_ENGAGED
                out.pointee.health = 0
                out.pointee.max_health = 120
                out.pointee.kills = 2
                out.pointee.stance = LockstepStance.attack.rawValue
                out.pointee.target_kind = LockstepTargetKind.structure.rawValue
                out.pointee.target = 1
                return CY_RESULT_OK
            }
            table.lockstep_structure = { _, index, out in
                guard let out, index == 1 else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no such structure")
                }
                out.pointee.health = 0
                out.pointee.max_health = 900
                out.pointee.radius = 3 << 32
                return CY_RESULT_OK
            }
            // Events 40, 41 and 42: a launch, an impact and a damage with no unit.
            table.lockstep_events = { _, first, out, capacity, count in
                guard let count else { return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "") }
                Seen.firstAsked = first
                let kinds: [LockstepEventKind] = [.launch, .impact, .damage]
                let available = (UInt64(40)...42).filter { $0 >= first }
                guard let out else {
                    count.pointee = UInt32(available.count)
                    return CY_RESULT_OK
                }
                var written: UInt32 = 0
                for sequence in available where written < capacity {
                    var event = CyLockstepEvent()
                    event.sequence = sequence
                    event.kind = kinds[Int(sequence - 40)].rawValue
                    event.unit = sequence == 42 ? CY_LOCKSTEP_NO_UNIT : 7
                    event.amount = 11
                    event.position = CyFixedVec2(x: 5 << 32, y: -(1 << 31))
                    out[Int(written)] = event
                    written += 1
                }
                count.pointee = written
                return CY_RESULT_OK
            }
            table.lockstep_projectiles = { _, out, capacity, count in
                guard let count else { return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "") }
                guard let out else {
                    count.pointee = 1
                    return CY_RESULT_OK
                }
                if capacity > 0 {
                    out[0] = CyLockstepProjectile()
                    out[0].id = 9
                    out[0].target_kind = LockstepTargetKind.unit.rawValue
                    out[0].target = 4
                    out[0].aim = CyFixedVec2(x: 1, y: 2)
                }
                count.pointee = min(capacity, 1)
                return CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testArmingSendsEveryFieldAsRawBits() throws {
        let half = Fixed(raw: Fixed.oneRaw / 2)
        try Lockstep.arm(
            5,
            .init(
                team: 2, health: 150, armour: 2, weapon: .ranged, damage: 30, cooldown: 150,
                range: Fixed(20), sight: Fixed(21), projectileSpeed: Fixed(15),
                splash: Fixed(2) + half))
        XCTAssertEqual(Seen.armedUnit, 5)
        XCTAssertEqual(Seen.arms.struct_size, UInt32(MemoryLayout<CyLockstepCombatDesc>.size))
        XCTAssertEqual(Seen.arms.team, 2)
        XCTAssertEqual(Seen.arms.weapon, CY_LOCKSTEP_WEAPON_RANGED.rawValue)
        XCTAssertEqual(Seen.arms.cooldown, 150)
        XCTAssertEqual(Seen.arms.range, 20 << 32)
        XCTAssertEqual(Seen.arms.splash, (5 << 32) / 2)
        XCTAssertEqual(Seen.arms.reserved, 0)
    }

    func testARefusalThrowsItsStatus() {
        XCTAssertThrowsError(
            try Lockstep.arm(
                0,
                .init(
                    team: 1, health: 0, weapon: .melee, damage: 1, cooldown: 1, range: .zero,
                    sight: .zero))
        ) { error in
            XCTAssertEqual(
                error as? CyberdyneError, .status(.invalidArgument, message: "no health"))
        }
    }

    func testFortifyingHandsBackTheIndex() throws {
        let index = try Lockstep.fortify(
            .init(
                team: 2, position: FixedVec2(x: Fixed(42), y: Fixed(42)), radius: Fixed(2),
                health: 900, armour: 4))
        XCTAssertEqual(index, 1)
        XCTAssertEqual(Seen.fortified.position.x, 42 << 32)
        XCTAssertEqual(Seen.fortified.health, 900)
        XCTAssertEqual(Seen.fortified.armour, 4)
    }

    func testTheAttackOrdersCarryTheirKindsAndPoints() throws {
        try Lockstep.order(.attackMove(group: 3, to: FixedVec2(x: Fixed(12), y: Fixed(14))))
        XCTAssertEqual(Seen.order.kind, LockstepOrderKind.attackMove.rawValue)
        XCTAssertEqual(Seen.order.group, 3)
        XCTAssertEqual(Seen.order.target.y, 14 << 32)
        try Lockstep.order(.attack(group: 0, at: FixedVec2(x: Fixed(42), y: Fixed(42))))
        XCTAssertEqual(Seen.order.kind, LockstepOrderKind.attack.rawValue)
        XCTAssertEqual(Seen.order.target.x, 42 << 32)
    }

    func testACombatantAndAStructureAreRead() throws {
        let fighter = try Lockstep.combatant(3)
        XCTAssertTrue(fighter.armed)
        XCTAssertFalse(fighter.alive)
        XCTAssertTrue(fighter.engaged)
        XCTAssertEqual(fighter.maxHealth, 120)
        XCTAssertEqual(fighter.kills, 2)
        XCTAssertEqual(fighter.stance, .attack)
        XCTAssertEqual(fighter.target, .structure(1))
        XCTAssertThrowsError(try Lockstep.combatant(4))
        let tower = try Lockstep.structure(1)
        XCTAssertFalse(tower.standing)
        XCTAssertEqual(tower.radius, Fixed(3))
    }

    func testEventsAreReadFromACursorEachOnce() throws {
        let all = try Lockstep.events(from: 0)
        XCTAssertEqual(all.map(\.sequence), [40, 41, 42])
        XCTAssertEqual(all.map(\.kind), [.launch, .impact, .damage])
        XCTAssertEqual(all[0].unit, 7)
        XCTAssertNil(all[2].unit)
        XCTAssertEqual(all[0].position.y, Fixed(raw: -(1 << 31)))
        let rest = try Lockstep.events(from: 42)
        XCTAssertEqual(Seen.firstAsked, 42)
        XCTAssertEqual(rest.map(\.sequence), [42])
        XCTAssertTrue(try Lockstep.events(from: 43).isEmpty)
    }

    func testProjectilesInFlight() throws {
        let flying = try Lockstep.projectiles
        XCTAssertEqual(flying.count, 1)
        XCTAssertEqual(flying[0].id, 9)
        XCTAssertEqual(flying[0].target, .unit(4))
        XCTAssertEqual(flying[0].aim, FixedVec2(x: Fixed(raw: 1), y: Fixed(raw: 2)))
    }
}
