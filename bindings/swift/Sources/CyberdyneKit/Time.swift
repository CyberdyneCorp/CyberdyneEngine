// SPDX-License-Identifier: MIT
// Time.swift — ABI 1.3's `time_get`, as a game writes it. `add-swift-game-api`.
//
// OWNER: implementer C (audio, spawning and time). See openspec/changes/add-swift-game-api/design.md.
//
//     let tick = try Time.now.tick                   // any phase
//     if try Time.phase == .fixedUpdate { … }
//
// DETERMINISM. Inside `onFixedUpdate` the engine reports `frameDelta` and `interpolation` as zero,
// so a fixed step cannot come to depend on the frame rate even by accident. Use `fixedDelta` there;
// `frameDelta` and `interpolation` are for `onUpdate`, where presentation smooths between ticks.

import CyberdyneABI
import CyberdyneCore

/// The engine's clock at the moment of the call.
public struct GameTime: Equatable, Sendable {
    /// The update phase the call was made in.
    public var phase: Phase
    /// In a fixed step, the tick being simulated; otherwise the last committed tick.
    public var tick: UInt64
    /// Seconds per fixed step.
    public var fixedDelta: Double
    /// Seconds since the previous frame. Zero in a fixed step.
    public var frameDelta: Double
    /// How far the frame is between the last committed tick and the next, in `[0, 1)`. Zero in a
    /// fixed step.
    public var interpolation: Double
    /// The tick is a resimulation (rollback or replay catch-up): sounds and camera moves issued now
    /// are accepted and dropped.
    public var isResimulating: Bool
    /// The simulation is paused; frames still run.
    public var isPaused: Bool

    /// From the ABI's spelling.
    public init(_ raw: CyTime) {
        phase = Phase(rawValue: raw.phase) ?? .none
        tick = raw.tick
        fixedDelta = raw.fixed_delta
        frameDelta = raw.frame_delta
        interpolation = raw.interpolation
        isResimulating = (raw.flags & CY_TIME_RESIMULATING) != 0
        isPaused = (raw.flags & CY_TIME_PAUSED) != 0
    }
}

/// The engine's clock. Never refused by phase: it is how a caller learns which phase it is in.
public enum Time {
    /// The clock now.
    public static var now: GameTime {
        get throws {
            var raw = CyTime()
            raw.struct_size = UInt32(MemoryLayout<CyTime>.size)
            try GameServices.engine().timeGet(into: &raw)
            return GameTime(raw)
        }
    }

    /// The phase the caller is in.
    public static var phase: Phase {
        get throws { try now.phase }
    }
}
