// SPDX-License-Identifier: MIT
// FogOfWar.swift — ABI 1.10's fog of war, as a game writes it.
//
//     // onCreate, or a fixed step: who sees, and how far.
//     try FogOfWar.setVision(.init(team: 1, revealRadius: Fixed(8)), of: scout)
//     try FogOfWar.setVision(.init(team: 2, revealRadius: Fixed(6), leavesGhost: true), of: keep)
//
//     // onUpdate: what the player may be shown.
//     if try FogOfWar.cell(team: 1, at: target) == .unexplored { hint = "unexplored" }
//     switch try FogOfWar.sight(team: 1, of: raider) {
//     case .visible: dot(at: raider.position)        // on the minimap where it is
//     case .ghost:   dot(at: lastSeen, dimmed: true) // a structure, where it was last seen
//     case .hidden:  break                           // not drawn at all
//     }
//
// The grid is the engine's `cy::gameplay::fog::FogOfWar`, updated by the host after each fixed step
// from where each entity with a vision stands. It is SIMULATION: a reveal radius is a `Fixed`, a
// position a `FixedVec2` (`y` is world Z), and every answer is the same on every peer of a lockstep
// session. So `setVision` is refused in `onUpdate` with `CyberdyneError.status(.permissionDenied,
// …)` — a frame must not decide what a player knows — while the questions may be asked in any
// phase: a frame reads the grid to draw it, which is all the renderer does with it.

import CyberdyneABI
import CyberdyneCore

/// The engine's fog of war, through ABI 1.10's `fog_*`.
public enum FogOfWar {
    /// An entity's part in the fog.
    public struct Vision: Equatable, Sendable {
        /// The team whose sight it gives, 1 to 32. Zero gives no sight.
        public var team: UInt32
        /// How far it sees, in metres.
        public var revealRadius: Fixed
        /// A structure: another team that saw it remembers it as a ghost.
        public var leavesGhost: Bool

        /// A vision.
        public init(team: UInt32, revealRadius: Fixed, leavesGhost: Bool = false) {
            self.team = team
            self.revealRadius = revealRadius
            self.leavesGhost = leavesGhost
        }

        /// From the ABI's spelling.
        public init(_ raw: CyFogVision) {
            team = raw.team
            revealRadius = Fixed(raw: raw.reveal_radius)
            leavesGhost = raw.flags & CY_FOG_LEAVES_GHOST != 0
        }

        /// In the ABI's spelling.
        public var raw: CyFogVision {
            var out = CyFogVision()
            out.struct_size = UInt32(MemoryLayout<CyFogVision>.size)
            out.team = team
            out.reveal_radius = revealRadius.raw
            out.flags = leavesGhost ? CY_FOG_LEAVES_GHOST : 0
            return out
        }
    }

    /// The grid and its last update.
    public struct Status: Equatable, Sendable {
        /// Cells along x.
        public var width: UInt32
        /// Cells along z.
        public var height: UInt32
        /// Entities with a vision the last update placed.
        public var units: UInt32
        /// The tick of the last update.
        public var tick: UInt64
        /// Cells some unit sees, counted once.
        public var visibleCells: UInt32
        /// Structures remembered as ghosts, every team's.
        public var ghosts: UInt32
        /// Equal on every peer after the same updates.
        public var digest: UInt64

        /// From the ABI's spelling.
        public init(_ raw: CyFogStatus) {
            width = raw.width
            height = raw.height
            units = raw.units
            tick = raw.tick
            visibleCells = raw.visible_cells
            ghosts = raw.ghosts
            digest = raw.digest
        }
    }

    /// Give `entity` a vision, from the next update. Throws `.invalidArgument` for a team past 32
    /// or a negative radius, and `.notFound` for an entity the host does not place.
    public static func setVision(_ vision: Vision, of entity: Entity) throws {
        var raw = vision.raw
        try GameServices.engine().fogSetVision(entity: entity.bits, vision: &raw)
    }

    /// The vision `entity` was given. Throws `.notFound` for an entity never given one.
    public static func vision(of entity: Entity) throws -> Vision {
        var raw = CyFogVision()
        raw.struct_size = UInt32(MemoryLayout<CyFogVision>.size)
        try GameServices.engine().fogGetVision(entity: entity.bits, into: &raw)
        return Vision(raw)
    }

    /// How far `entity` sees, in metres: its vision's reveal radius.
    public static func revealRadius(of entity: Entity) throws -> Fixed {
        try vision(of: entity).revealRadius
    }

    /// How `team` knows the cell holding `position` (`y` is world Z). Off the grid is
    /// `.unexplored`.
    public static func cell(team: UInt32, at position: FixedVec2) throws -> FogCell {
        var point = position.abi
        var raw: UInt32 = 0
        try GameServices.engine().fogCell(team: team, position: &point, into: &raw)
        return FogCell(rawValue: raw) ?? .unexplored
    }

    /// Whether `team` sees `position` now.
    public static func isVisible(team: UInt32, at position: FixedVec2) throws -> Bool {
        try cell(team: team, at: position) == .visible
    }

    /// How `team` sees `entity`: `.visible`, a structure's `.ghost`, or `.hidden` — not drawn.
    public static func sight(team: UInt32, of entity: Entity) throws -> FogSight {
        var raw: UInt32 = 0
        try GameServices.engine().fogSight(team: team, entity: entity.bits, into: &raw)
        return FogSight(rawValue: raw) ?? .hidden
    }

    /// The grid and its last update.
    public static var status: Status {
        get throws {
            var raw = CyFogStatus()
            raw.struct_size = UInt32(MemoryLayout<CyFogStatus>.size)
            try GameServices.engine().fogStatus(into: &raw)
            return Status(raw)
        }
    }
}
