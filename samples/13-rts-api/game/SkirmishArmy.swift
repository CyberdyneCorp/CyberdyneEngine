// SPDX-License-Identifier: MIT
// SkirmishArmy.swift — the battle's units as the skirmish reads them, the field's edges, the
// formation a group of units takes round the point it is sent to, and who has won.
//
// Everything the session decides is read here, never decided: where a unit stands, whether it is
// alive, how much health it has left, whether a building stands. The skirmish keeps a frame's
// snapshot of it — read once, projected to the screen once — and every pick, box and order of that
// frame works from the same picture.

import CyberdyneKit

/// One unit as a frame sees it.
struct Trooper {
    /// The unit's place in `Battle.units`: its index in every per-unit array of the battle.
    let index: Int
    /// The session's unit, and the group an order addresses it by.
    let unit: UInt32
    let group: UInt32
    let kind: Battle.Kind
    let soldier: Entity
    /// Its feet: in the field's own `Fixed` coordinates, and in the level's metres.
    let field: FixedVec2
    let at: Vec3
    let alive: Bool
    let health: UInt32
    let maxHealth: UInt32
    /// Where its chest is on screen, when the camera sees it.
    var screen: Vec2?

    var team: UInt32 { kind.team }
    var isBlue: Bool { kind.team == Battle.blue }

    /// What the selection panel calls it.
    var name: String {
        switch kind {
        case .blueSword: return "Swordsman"
        case .blueArcher: return "Archer"
        case .catapult: return "Catapult"
        case .redSword: return "Red swordsman"
        case .redArcher: return "Red archer"
        }
    }
}

/// The battle's units, read once a frame.
enum SkirmishArmy {
    /// Half a soldier's height: a pick aims at the chest, as a player does.
    static let chest: Float = 0.9

    /// Every unit, in enlistment order.
    static func read(_ battle: Battle) throws -> [Trooper] {
        var troops: [Trooper] = []
        troops.reserveCapacity(battle.units.count)
        for (index, unit) in battle.units.enumerated() {
            let state = try Lockstep.unit(unit)
            let fighter = try Lockstep.combatant(unit)
            troops.append(
                Trooper(
                    index: index, unit: unit, group: battle.groups[index],
                    kind: battle.kinds[index], soldier: battle.soldiers[index],
                    field: state.position, at: Battle.level(state.position, height: 0),
                    alive: fighter.alive, health: fighter.health, maxHealth: fighter.maxHealth))
        }
        return troops
    }

    /// Where each living unit's chest is on screen, through `camera`.
    static func project(_ troops: inout [Trooper], through camera: Camera) throws {
        let living = troops.indices.filter { troops[$0].alive }
        let chests = living.map { index -> Vec3 in
            let at = troops[index].at
            return Vec3(x: at.x, y: at.y + chest, z: at.z)
        }
        guard !chests.isEmpty else { return }
        let points = try camera.project(chests)
        for (slot, index) in living.enumerated() where points[slot].isOnScreen {
            troops[index].screen = points[slot].position
        }
    }

    // --- The field ------------------------------------------------------------------------------

    /// The field's walkable square, in its own metres: the company field's 48, a metre in from
    /// each edge so a formation's slots stay on it.
    static let fieldLow = Fixed(1)
    static let fieldHigh = Fixed(47)

    /// `point` moved onto the field.
    static func clamp(_ point: FixedVec2) -> FixedVec2 {
        FixedVec2(
            x: min(max(point.x, fieldLow), fieldHigh), y: min(max(point.y, fieldLow), fieldHigh))
    }

    /// The level's metres as the field's `Fixed` — the one place a float becomes a lockstep
    /// value. The issuer converts once; every peer then reads these bits from the command log.
    static func field(_ level: Vec3) -> FixedVec2 {
        clamp(
            FixedVec2(
                x: Fixed(cooking: level.x - Battle.origin.x),
                y: Fixed(cooking: level.z - Battle.origin.z)))
    }

    /// The slots of a square formation of `count` round `centre`, two and a quarter metres apart —
    /// the session's own formation spacing — row by row. Integer and `Fixed` arithmetic only, so
    /// the bits are the same on every machine that computes them.
    static func formation(_ count: Int, around centre: FixedVec2) -> [FixedVec2] {
        guard count > 0 else { return [] }
        var columns = 1
        while columns * columns < count {
            columns += 1
        }
        let rows = (count + columns - 1) / columns
        return (0..<count).map { slot in
            let column = Int32(slot % columns)
            let row = Int32(slot / columns)
            let dx = Battle.quarters((2 * column - Int32(columns - 1)) * 9) >> 1
            let dz = Battle.quarters((2 * row - Int32(rows - 1)) * 9) >> 1
            return clamp(FixedVec2(x: centre.x + dx, y: centre.y + dz))
        }
    }

    /// The square of the distance between two field points, in `Fixed`.
    static func distanceSquared(_ a: FixedVec2, _ b: FixedVec2) -> Fixed {
        let dx = a.x - b.x
        let dz = a.y - b.y
        return dx * dx + dz * dz
    }
}

/// How the skirmish stands.
enum Outcome: UInt32 {
    case playing = 0
    /// Both of red's buildings have fallen.
    case victory = 1
    /// No blue unit is left alive.
    case defeat = 2

    /// From the session's state: the units' lives and the buildings' health.
    static func judge(_ troops: [Trooper], structures: [UInt32]) throws -> Outcome {
        if !troops.contains(where: { $0.isBlue && $0.alive }) {
            return .defeat
        }
        for structure in structures {
            if try Lockstep.structure(structure).standing {
                return .playing
            }
        }
        return .victory
    }
}
