// SPDX-License-Identifier: MIT
// Lockstep.swift — a Swift game's orders, through the engine's lockstep path. ABI 1.8,
// openspec/changes/add-deterministic-math stage 8.
//
//     let unit = try Lockstep.enlist(.init(group: 0, entity: worker, position: spawn))  // onCreate
//     try Lockstep.order(.move(group: 0, to: FixedVec2(x: Fixed(24), y: Fixed(8))))   // a step
//     let state = try Lockstep.unit(unit)                                              // anywhere
//     let digest = try Lockstep.status.digest
//
// WHAT RUNS WHERE. The session is the host's: `cy::game_backend::LockstepAdapter`, a fixed-point
// world (`cy::movement`'s mover, its converted navigation mesh, A* and the funnel, and crowd
// avoidance instantiated over `Fixed`) behind a `cy::gameplay::CommandStream` running under
// `Lockstep`. An ORDER is a command in that stream whose payload is raw `Fixed` values, recorded for
// the next tick; every peer that executes the same log computes the same bits, on any architecture.
// A unit is identified by its enlistment index; its entity is only where it is drawn.
//
// PHASES. Enlisting is session configuration, before the first tick (`onCreate`). An order may be
// recorded in a fixed step or a frame — a click converted once with `Fixed(cooking:)` by the peer
// that issued it is exactly the conversion design §7.1 permits. Reading state answers anywhere.

import CyberdyneABI
import CyberdyneCore

/// The engine's lockstep session, through ABI 1.8's `lockstep_*`.
public enum Lockstep {
    /// A unit to enlist. Zero radius and speed are the engine's defaults (0.5 m, 4 m/s).
    public struct UnitDesc: Equatable, Sendable {
        /// The group an order addresses.
        public var group: UInt32
        /// The scene entity that draws the unit, or `.null`; not hashed.
        public var entity: Entity
        /// Where it starts: x and world Z.
        public var position: FixedVec2
        /// Its radius; zero is the engine's default.
        public var radius: Fixed
        /// Its top speed; zero is the engine's default.
        public var maxSpeed: Fixed

        /// A unit to enlist.
        public init(
            group: UInt32, entity: Entity = .null, position: FixedVec2,
            radius: Fixed = .zero, maxSpeed: Fixed = .zero
        ) {
            self.group = group
            self.entity = entity
            self.position = position
            self.radius = radius
            self.maxSpeed = maxSpeed
        }
    }

    /// What a group is told to do.
    public enum Order: Equatable, Sendable {
        /// Plan a path to `target` and form up around it.
        case move(group: UInt32, to: FixedVec2)
        /// Drop the path and stand.
        case stop(group: UInt32)

        var abi: CyLockstepOrder {
            var raw = CyLockstepOrder()
            raw.struct_size = UInt32(MemoryLayout<CyLockstepOrder>.size)
            switch self {
            case .move(let group, let target):
                raw.kind = LockstepOrderKind.move.rawValue
                raw.group = group
                raw.target = target.abi
            case .stop(let group):
                raw.kind = LockstepOrderKind.stop.rawValue
                raw.group = group
            }
            return raw
        }
    }

    /// A unit's authoritative state as the last tick left it.
    public struct Unit: Equatable, Sendable {
        /// The group it is in.
        public var group: UInt32
        /// The entity that draws it.
        public var entity: Entity
        /// Where it stands: x and world Z.
        public var position: FixedVec2
        /// Its velocity on the plane.
        public var velocity: FixedVec2
        /// Its height, from the navigation surface.
        public var height: Fixed
        /// Which way it faces, a yaw about +Y.
        public var heading: Angle
        /// It has a path it has not finished.
        public var moving: Bool
        /// It finished its path on the last tick, and only then.
        public var justArrived: Bool

        init(_ raw: CyLockstepUnit) {
            group = raw.group
            entity = Entity(bits: raw.entity)
            position = FixedVec2(raw.position)
            velocity = FixedVec2(raw.velocity)
            height = Fixed(raw: raw.height)
            heading = Angle(raw: raw.heading)
            moving = raw.flags & CY_LOCKSTEP_UNIT_MOVING != 0
            justArrived = raw.flags & CY_LOCKSTEP_UNIT_ARRIVED != 0
        }
    }

    /// The session.
    public struct Status: Equatable, Sendable {
        /// Units enlisted.
        public var units: UInt32
        /// Ticks advanced: the next tick to run has this number.
        public var tick: UInt64
        /// Commands in the session's log.
        public var commands: UInt64
        /// The last tick's state hash.
        public var stateHash: UInt64
        /// Every tick's state hash folded in order: equal digests, equal sessions.
        public var digest: UInt64
        /// `cy::detmath::kKernelVersion`: another number is another kernel.
        public var kernelVersion: UInt32
        /// Ticks on which a follower peer disagreed; zero without one.
        public var disagreements: UInt32
    }

    /// Enlist a unit, before the first tick. Returns its index, its identity in the session.
    @discardableResult
    public static func enlist(_ desc: UnitDesc) throws -> UInt32 {
        let engine = try GameServices.engine()
        var raw = CyLockstepUnitDesc()
        raw.struct_size = UInt32(MemoryLayout<CyLockstepUnitDesc>.size)
        raw.group = desc.group
        raw.entity = desc.entity.bits
        raw.position = desc.position.abi
        raw.radius = desc.radius.raw
        raw.max_speed = desc.maxSpeed.raw
        var index: UInt32 = 0
        try engine.lockstepEnlist(desc: &raw, into: &index)
        return index
    }

    /// Record `order` for the session's next tick.
    public static func order(_ order: Order) throws {
        let engine = try GameServices.engine()
        var raw = order.abi
        try engine.lockstepOrder(order: &raw)
    }

    /// Unit `index`'s state.
    public static func unit(_ index: UInt32) throws -> Unit {
        let engine = try GameServices.engine()
        var raw = CyLockstepUnit()
        raw.struct_size = UInt32(MemoryLayout<CyLockstepUnit>.size)
        try engine.lockstepUnit(unit: index, into: &raw)
        return Unit(raw)
    }

    /// The session's tick, log, hash and digest.
    public static var status: Status {
        get throws {
            let engine = try GameServices.engine()
            var raw = CyLockstepStatus()
            raw.struct_size = UInt32(MemoryLayout<CyLockstepStatus>.size)
            try engine.lockstepStatus(into: &raw)
            return Status(
                units: raw.units, tick: raw.tick, commands: raw.commands,
                stateHash: raw.state_hash, digest: raw.digest,
                kernelVersion: raw.kernel_version, disagreements: raw.disagreements)
        }
    }
}
