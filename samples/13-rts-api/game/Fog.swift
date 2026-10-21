// SPDX-License-Identifier: MIT
// Fog.swift — what each side sees, through ABI 1.10 (`FogOfWar`).
//
// The host keeps the grid (host/fog.h): after every fixed step it updates it from where each entity
// with a vision stands. Every decision about who sees, how far, and what the player is shown is
// here. The commander:
//
//   onReady        gives its squad and its barracks blue's sight, and the camp it resolved by
//                  `@Node` — two raiders and a watchtower — red's; the barracks and the tower leave
//                  ghosts, as structures do;
//   a fixed step   gives each worker it builds blue's sight as it enlists it — the grid is
//                  simulation, so a fixed step is where it changes;
//   onUpdate       reads the grid to draw it: which of the camp blue sees, which it remembers, and
//                  how many cells it sees — and is refused once when it tries to give a vision
//                  from a frame, which would let presentation decide what a player knows.
//
// What it reads goes on the HUD's minimap — a raider's dot only while blue sees it, the tower's
// where it was last seen — and into `RtsReport` for the sample's report.

import CyberdyneKit

struct BaseFog {
    /// The teams, as the grid numbers them.
    static let blue: UInt32 = 1
    static let red: UInt32 = 2
    /// How far each kind sees, in metres.
    static let workerSight = Fixed(7)
    static let barracksSight = Fixed(5)
    static let raiderSight = Fixed(6)
    static let towerSight = Fixed(8)

    private(set) var camp: [(entity: Entity, at: Vec3, structure: Bool)] = []
    /// The first worker's reveal radius, read back through the ABI, in metres.
    private(set) var radius: Float = 0
    /// Cells blue's side sees on the last frame.
    private(set) var cells: Float = 0
    /// The tower was ever seen, and is a ghost now.
    private(set) var towerSeen: Float = 0
    private(set) var towerGhost: Float = 0
    /// Raiders blue sees on the last frame, and whether the far raider was hidden on the first.
    private(set) var raidersSeen: Float = 0
    private(set) var farHiddenAtStart: Float = 0
    /// Enemy dots the minimap showed on the last frame.
    private(set) var enemyDots: Float = 0
    /// Visions a frame tried to give and was refused.
    private(set) var refused: Float = 0
    private var frames: UInt64 = 0

    /// `onReady`: every `@Node` is resolved, and no phase is set, which `fog_set_vision` accepts.
    mutating func create(
        squad: [Entity], barracks: Entity?, raiders: [Entity?], tower: Entity?
    ) throws {
        for unit in squad {
            try enlist(unit)
        }
        if let barracks {
            try FogOfWar.setVision(
                .init(team: Self.blue, revealRadius: Self.barracksSight, leavesGhost: true),
                of: barracks)
        }
        for (raider, at) in zip(raiders, Camp.raiders) {
            guard let raider else { continue }
            try FogOfWar.setVision(
                .init(team: Self.red, revealRadius: Self.raiderSight), of: raider)
            camp.append((raider, at, false))
        }
        if let tower {
            try FogOfWar.setVision(
                .init(team: Self.red, revealRadius: Self.towerSight, leavesGhost: true), of: tower)
            camp.append((tower, Camp.tower, true))
        }
        if let first = squad.first {
            radius = Float(try FogOfWar.revealRadius(of: first).presentation)
        }
    }

    /// A worker joining the squad sees for blue from the next update.
    func enlist(_ unit: Entity) throws {
        try FogOfWar.setVision(.init(team: Self.blue, revealRadius: Self.workerSight), of: unit)
    }

    /// `onUpdate`: what blue knows of the camp, and the camp's dots for the minimap.
    mutating func frame(mapSize: Float) throws -> [HudModel.Dot] {
        frames += 1
        if frames == 1, let first = camp.first {
            do {
                try FogOfWar.setVision(
                    .init(team: Self.blue, revealRadius: Fixed(1)), of: first.entity)
            } catch CyberdyneError.status(.permissionDenied, _) {
                refused += 1
            }
        }
        cells = Float(try FogOfWar.status.visibleCells)
        var dots: [HudModel.Dot] = []
        var seen: Float = 0
        for member in camp {
            let sight = try FogOfWar.sight(team: Self.blue, of: member.entity)
            if member.structure {
                towerSeen = sight == .visible ? 1 : towerSeen
                towerGhost = sight == .ghost ? 1 : 0
            } else if sight == .visible {
                seen += 1
            }
            if frames == 1, !member.structure, member.at == Camp.raiders.last {
                farHiddenAtStart = sight == .hidden ? 1 : 0
            }
            // A raider only while blue sees it; the tower where it was last seen. It never moves,
            // so where it stands is where it was seen.
            if sight != .hidden {
                dots.append(.init(x: member.at.x / mapSize, y: member.at.z / mapSize, team: .enemy))
            }
        }
        raidersSeen = seen
        enemyDots = Float(dots.count)
        return dots
    }
}
