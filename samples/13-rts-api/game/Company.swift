// SPDX-License-Identifier: MIT
// Company.swift — the commander's lockstep company: unit orders through the engine's lockstep path,
// computed in fixed point. ABI 1.8, openspec/changes/add-deterministic-math stage 8.
//
// Two groups of eight units on a field of their own, which the host runs as a lockstep session: a
// fixed-point world behind a command stream, with a second peer that sees only the first one's
// command log. The commander enlists the units before the first tick and, every `orderEvery` ticks,
// sends each group to the next waypoint of a patrol round the field.
//
// EVERY NUMBER HERE IS BITS. A waypoint is `centre + (cos a, sin a) * radius`: the angle `a` is
// integer arithmetic on a binary `Angle`, its cosine and sine are the engine's own polynomials
// (`Detmath.cos`, `Detmath.sin`, ABI 1.8), and the products and sums are `Fixed`'s `*` and `+`,
// which round and wrap exactly as the engine's do. host/company.h computes the same waypoints in
// C++, and `integration.rts_api_sample` checks that the session this Swift drives ends in the same
// digest every CI leg computes from those C++ orders (`determinism.cross_leg`'s
// `detmath-company-digest`). Nothing a float decided reaches the session.

import CyberdyneKit

struct Company {
    /// host/company.h's constants, value for value.
    static let groups: UInt32 = 2
    static let groupSize: UInt32 = 8
    static let orderEvery: UInt64 = 90
    static let centre = Fixed(24)
    static let radius = Fixed(14)
    static let waypointTurn: UInt32 = 0x3333_3333
    static let groupTurn: UInt32 = 0x8000_0000
    static let spacing = Fixed(raw: (Fixed.oneRaw * 9) / 4)

    /// Where unit `index` of `group` starts: a 4 x 2 block, the first group south-west, the second
    /// north-east. `company::spawn`.
    static func spawn(group: UInt32, index: UInt32) -> FixedVec2 {
        let origin = Fixed(group == 0 ? 4 : 36)
        return FixedVec2(
            x: origin + Fixed(Int32(index % 4)) * spacing,
            y: origin + Fixed(Int32(index / 4)) * spacing)
    }

    /// Where `group` is sent by the order of `tick`. `company::waypoint`.
    static func waypoint(tick: UInt64, group: UInt32) -> FixedVec2 {
        let order = UInt32(truncatingIfNeeded: tick / orderEvery)
        let angle = Angle(raw: order &* waypointTurn &+ group &* groupTurn)
        return FixedVec2(
            x: centre + Detmath.cos(angle) * radius, y: centre + Detmath.sin(angle) * radius)
    }

    /// The units' indices in the session, in enlistment order.
    private(set) var units: [UInt32] = []
    /// Orders given, and arrivals seen: a unit's one-tick ARRIVED flag.
    private(set) var orders: Float = 0
    private(set) var arrivals: Float = 0
    /// The first unit's x, read back from the session: authoritative state, a `Fixed`.
    private(set) var lead = Fixed.zero

    /// Enlist both groups. False, and said once, when the host runs no lockstep session.
    mutating func enlist() throws -> Bool {
        do {
            for group in 0..<Company.groups {
                for index in 0..<Company.groupSize {
                    units.append(
                        try Lockstep.enlist(
                            .init(group: group, position: Company.spawn(group: group, index: index))
                        ))
                }
            }
            return true
        } catch CyberdyneError.status(.unavailable, _) {
            Log.info("Commander: the engine runs no lockstep session; no company")
            return false
        }
    }

    /// One fixed step, before the session runs its tick: hear who arrived on the last one, and on
    /// an order tick send every group on.
    mutating func step() throws {
        guard !units.isEmpty else { return }
        for unit in units {
            if try Lockstep.unit(unit).justArrived {
                arrivals += 1
            }
        }
        lead = try Lockstep.unit(units[0]).position.x
        let tick = try Lockstep.status.tick
        guard tick % Company.orderEvery == 0 else { return }
        for group in 0..<Company.groups {
            try Lockstep.order(.move(group: group, to: Company.waypoint(tick: tick, group: group)))
            orders += 1
        }
    }
}
