// SPDX-License-Identifier: MIT
// RedOpponent.swift — the opponent the player fights: red's army, ordered from a fixed step through
// the same `Lockstep.order` the player's clicks become.
//
// It thinks every half second, from the session's state alone — where every unit stands, who is
// alive, whether its buildings are threatened — so it is as deterministic as the session: the same
// battle gives it the same thoughts on every machine, and its orders are commands in the same log
// the player's are, which is what lets a recorded game replay without it.
//
//   defend   a blue unit within 18 metres of a standing red building: every red unit that is not
//            already fighting attack-moves to the building
//   attack   red's army is the stronger, counted in units alive, or the player has dawdled for 25
//            seconds: it attack-moves on the blue unit nearest its own centre
//   hold     otherwise: a unit that has wandered more than ten metres from where it started walks
//            back, fighting what it meets, and the rest stand and fight what comes within sight
//
// A unit that is fighting is left alone, and a unit is given a new order at most every second and
// a half unless what it is doing changed, so the opponent does not drown its own units in orders.

import CyberdyneKit

struct RedOpponent {
    enum Posture: UInt32 {
        case hold = 0
        case defend = 1
        case attack = 2
    }

    static let thinkEvery: UInt64 = 30
    static let patience: UInt64 = 1500
    static let reorderAfter: UInt64 = 90
    static let threat = Fixed(18)
    static let straying = Fixed(10)

    private(set) var posture = Posture.hold
    /// Orders given, for the report.
    private(set) var orders: Float = 0
    /// Each unit's last order: the tick, and the posture it was given under.
    private var last: [Int: (tick: UInt64, posture: Posture)] = [:]
    /// Where each red unit started: where it holds.
    private var home: [Int: FixedVec2] = [:]

    /// One fixed step. Returns the orders it gave, which it has already recorded.
    mutating func step(
        tick: UInt64, troops: [Trooper], structures: [UInt32]
    ) throws -> [Lockstep.Order] {
        if home.isEmpty {
            for trooper in troops where !trooper.isBlue {
                home[trooper.index] = trooper.field
            }
        }
        guard tick % RedOpponent.thinkEvery == 0 else { return [] }
        let red = troops.filter { !$0.isBlue && $0.alive }
        let blue = troops.filter { $0.isBlue && $0.alive }
        guard !red.isEmpty, !blue.isEmpty else { return [] }
        let (posture, target) = try decide(tick: tick, red: red, blue: blue, structures)
        self.posture = posture
        var given: [Lockstep.Order] = []
        for trooper in red {
            guard let order = try order(for: trooper, posture: posture, target: target, tick: tick)
            else { continue }
            try Lockstep.order(order)
            last[trooper.index] = (tick, posture)
            orders += 1
            given.append(order)
        }
        return given
    }

    // --- Deciding --------------------------------------------------------------------------------

    private func decide(
        tick: UInt64, red: [Trooper], blue: [Trooper], _ structures: [UInt32]
    ) throws -> (Posture, FixedVec2) {
        let reach = RedOpponent.threat * RedOpponent.threat
        for structure in structures {
            let building = try Lockstep.structure(structure)
            guard building.standing else { continue }
            let threatened = blue.contains {
                SkirmishArmy.distanceSquared($0.field, building.position) < reach
            }
            if threatened {
                return (.defend, building.position)
            }
        }
        if tick >= RedOpponent.patience || red.count * 4 > blue.count * 5 {
            return (.attack, nearest(blue, to: centre(red)))
        }
        return (.hold, FixedVec2())
    }

    private func order(
        for trooper: Trooper, posture: Posture, target: FixedVec2, tick: UInt64
    ) throws -> Lockstep.Order? {
        let fighter = try Lockstep.combatant(trooper.unit)
        if fighter.engaged || fighter.chasing {
            return nil
        }
        if let previous = last[trooper.index], previous.posture == posture,
            tick < previous.tick + RedOpponent.reorderAfter
        {
            return nil
        }
        switch posture {
        case .defend, .attack:
            if let previous = last[trooper.index], previous.posture == posture,
                fighter.stance == .attackMove
            {
                return nil
            }
            return .attackMove(group: trooper.group, to: target)
        case .hold:
            guard let start = home[trooper.index] else { return nil }
            let away = RedOpponent.straying * RedOpponent.straying
            guard SkirmishArmy.distanceSquared(trooper.field, start) > away else { return nil }
            return .attackMove(group: trooper.group, to: start)
        }
    }

    private func centre(_ troops: [Trooper]) -> FixedVec2 {
        var x = Fixed.zero
        var z = Fixed.zero
        for trooper in troops {
            x += trooper.field.x
            z += trooper.field.y
        }
        let count = Fixed(Int32(troops.count))
        return FixedVec2(x: x / count, y: z / count)
    }

    private func nearest(_ troops: [Trooper], to point: FixedVec2) -> FixedVec2 {
        var best = troops[0].field
        var bestDistance = SkirmishArmy.distanceSquared(best, point)
        for trooper in troops.dropFirst() {
            let distance = SkirmishArmy.distanceSquared(trooper.field, point)
            if distance < bestDistance {
                best = trooper.field
                bestDistance = distance
            }
        }
        return best
    }
}
