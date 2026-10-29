// SPDX-License-Identifier: MIT
// SkirmishOrders.swift — what the player's hand means: which units a click or a box picks, the
// control groups, and the orders a right click gives.
//
// A FRAME DECIDES, A FIXED STEP ORDERS. The pointer and the camera are presentation, so a pick is
// made in `onUpdate`; an order changes the simulation, so it is RECORDED there and given in the
// next `onFixedUpdate` through `Lockstep.order` — the same command, in the same log, that the
// scripted battle's plan and the opponent's thoughts become. That is why a recorded game replays:
// the log is the whole of what anyone asked.
//
// The session addresses a group, never a unit, so a skirmish enlists every unit in a group of its
// own (`Battle.Mode.skirmish`) and an order to five units is five commands, each to the unit's
// slot in a square formation round the point clicked (`SkirmishArmy.formation`).

import CyberdyneKit

/// The units the player has picked, by their index in the battle.
struct SkirmishSelection {
    /// A click this close to a unit's chest, in pixels, picks it.
    static let pickRadius: Float = 18
    /// A drag longer than this, in pixels, is a box rather than a click.
    static let dragThreshold: Float = 6

    private(set) var chosen: [Int] = []
    /// Where the left button went down, while it is held.
    private(set) var anchor: Vec2?
    /// The box being dragged, when the press has moved far enough to be one.
    private(set) var box: UIRect?
    /// Times the selection changed, for the report.
    private(set) var changes: Float = 0

    mutating func press(at point: Vec2) {
        anchor = point
        box = nil
    }

    mutating func drag(to point: Vec2) {
        guard let start = anchor else { return }
        let width = abs(point.x - start.x)
        let height = abs(point.y - start.y)
        guard box != nil || max(width, height) > SkirmishSelection.dragThreshold else { return }
        box = UIRect(
            x: min(start.x, point.x), y: min(start.y, point.y), width: width, height: height)
    }

    /// The button came up: a box picks every living blue unit inside it, a click the one under the
    /// pointer, or nothing. With `adding` (shift) the picked units join the selection — a click on
    /// one already in it takes it out.
    mutating func release(at point: Vec2, troops: [Trooper], adding: Bool) {
        defer {
            anchor = nil
            box = nil
        }
        guard anchor != nil else { return }
        var picked: [Int]
        if let rect = box {
            picked = troops.filter { trooper in
                guard trooper.isBlue, trooper.alive, let at = trooper.screen else { return false }
                return rect.contains(at)
            }.map(\.index)
        } else if let one = SkirmishSelection.under(point, troops, team: Battle.blue) {
            picked = [one.index]
            if adding, let found = chosen.firstIndex(of: one.index) {
                chosen.remove(at: found)
                changes += 1
                return
            }
        } else {
            picked = []
        }
        set(adding ? chosen + picked.filter { !chosen.contains($0) } : picked)
    }

    /// Replace the selection.
    mutating func set(_ units: [Int]) {
        guard units != chosen else { return }
        chosen = units
        changes += 1
    }

    /// Forget the fallen.
    mutating func prune(_ troops: [Trooper]) {
        let living = chosen.filter { troops[$0].alive }
        if living.count != chosen.count {
            chosen = living
        }
    }

    /// The living unit of `team` whose chest is nearest `point` on screen, within the pick radius.
    static func under(_ point: Vec2, _ troops: [Trooper], team: UInt32) -> Trooper? {
        var best: Trooper?
        var bestDistance = pickRadius * pickRadius
        for trooper in troops where trooper.alive && trooper.team == team {
            guard let at = trooper.screen else { continue }
            let dx = at.x - point.x
            let dy = at.y - point.y
            let distance = dx * dx + dy * dy
            if distance <= bestDistance {
                best = trooper
                bestDistance = distance
            }
        }
        return best
    }
}

/// Nine control groups: Ctrl and a number keeps the selection, the number calls it back, and the
/// number twice within a third of a second also brings the camera to it.
struct ControlGroups {
    static let doubleTap: Double = 0.35

    private var groups: [[Int]] = Array(repeating: [], count: 9)
    private var lastCall: (group: Int, at: Double)?
    private(set) var assigned: Float = 0
    private(set) var recalled: Float = 0

    mutating func assign(_ group: Int, _ units: [Int]) {
        groups[group] = units
        assigned += 1
    }

    /// The group's living units, and whether this call is the second of a double tap.
    mutating func recall(
        _ group: Int, troops: [Trooper], now: Double
    ) -> (units: [Int], twice: Bool) {
        let units = groups[group].filter { troops[$0].alive }
        let twice =
            lastCall.map { $0.group == group && now - $0.at < ControlGroups.doubleTap } ?? false
        lastCall = (group, now)
        recalled += 1
        return (units, twice)
    }
}

/// An order the player gave in a frame, waiting for the next fixed step.
struct PlayerOrder {
    enum Kind: UInt32 {
        case move = 0
        case stop = 1
        case attackMove = 2
        case attack = 3
    }

    let kind: Kind
    /// The units, by index in the battle.
    let units: [Int]
    /// Where, on the field.
    let point: FixedVec2

    /// The commands it becomes: one a unit, each to its own group, and — for a move or an
    /// attack-move — each to its own slot of a formation round the point. An attack sends every
    /// unit at the same point, where the session picks the enemy standing nearest it.
    func commands(_ troops: [Trooper]) -> [Lockstep.Order] {
        let living = units.filter { troops[$0].alive }
        switch kind {
        case .stop:
            return living.map { .stop(group: troops[$0].group) }
        case .attack:
            return living.map { .attack(group: troops[$0].group, at: point) }
        case .move, .attackMove:
            let slots = SkirmishArmy.formation(living.count, around: point)
            return zip(living, slots).map { index, slot in
                kind == .move
                    ? .move(group: troops[index].group, to: slot)
                    : .attackMove(group: troops[index].group, to: slot)
            }
        }
    }
}
