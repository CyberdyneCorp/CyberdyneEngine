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
        /// ABI 1.16: go to `target`, fighting what comes within sight on the way.
        case attackMove(group: UInt32, to: FixedVec2)
        /// ABI 1.16: go for the enemy standing nearest `point` — an attack-move to it when none
        /// stands within 3 m of it.
        case attack(group: UInt32, at: FixedVec2)

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
            case .attackMove(let group, let target):
                raw.kind = LockstepOrderKind.attackMove.rawValue
                raw.group = group
                raw.target = target.abi
            case .attack(let group, let point):
                raw.kind = LockstepOrderKind.attack.rawValue
                raw.group = group
                raw.target = point.abi
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

// --- ABI 1.16: combat ----------------------------------------------------------------------------
//
//     try Lockstep.arm(unit, .init(team: 1, health: 120, weapon: .melee, damage: 16, cooldown: 36,
//                                  range: Fixed(raw: Fixed.oneRaw / 2), sight: Fixed(8)))  // onCreate
//     let tower = try Lockstep.fortify(.init(team: 2, position: at, radius: Fixed(2), health: 900))
//     try Lockstep.order(.attackMove(group: 0, to: FixedVec2(x: Fixed(30), y: Fixed(32))))  // a step
//     for event in try Lockstep.events(from: cursor) { ... }                  // a frame: effects
//
// The session decides every blow, flight, hit and death in integers and `Fixed`; a game READS what
// happened — from a frame update, where effects, lights and sounds may play — and never feeds it
// back. Events are numbered from zero in the order the session made them: keep the number after
// the last one read as a cursor, and each is read once however many ticks ran since.

extension Lockstep {
    /// An armed unit's health and weapon, as `arm` takes it. Reaches are the gap between the two
    /// footprints.
    public struct Arms: Equatable, Sendable {
        /// Units and structures of another team are enemies.
        public var team: UInt32
        /// What it starts with and can never exceed.
        public var health: UInt32
        /// Taken off every hit; a hit always deals at least one point.
        public var armour: UInt32
        /// Whether it strikes in melee or launches projectiles.
        public var weapon: LockstepWeapon
        /// Points one hit deals before the target's armour.
        public var damage: UInt32
        /// Ticks between two attacks.
        public var cooldown: UInt32
        /// How far it attacks across: the gap between its footprint and its target's.
        public var range: Fixed
        /// How far it notices an enemy to attack, the same gap.
        public var sight: Fixed
        /// Metres a second, for a ranged weapon.
        public var projectileSpeed: Fixed
        /// A ranged weapon's splash radius; zero hits the target alone.
        public var splash: Fixed

        /// Arms; the armour, the speed and the splash default to none.
        public init(
            team: UInt32, health: UInt32, armour: UInt32 = 0, weapon: LockstepWeapon,
            damage: UInt32, cooldown: UInt32, range: Fixed, sight: Fixed,
            projectileSpeed: Fixed = .zero, splash: Fixed = .zero
        ) {
            self.team = team
            self.health = health
            self.armour = armour
            self.weapon = weapon
            self.damage = damage
            self.cooldown = cooldown
            self.range = range
            self.sight = sight
            self.projectileSpeed = projectileSpeed
            self.splash = splash
        }
    }

    /// A building to place: a static target units walk round.
    public struct StructureDesc: Equatable, Sendable {
        /// Units of another team attack it.
        public var team: UInt32
        /// The scene entity that draws it, or `.null`; not hashed.
        public var entity: Entity
        /// Its centre: x and world Z.
        public var position: FixedVec2
        /// Its footprint, a circle units walk round.
        public var radius: Fixed
        /// What it takes to knock it down.
        public var health: UInt32
        /// Taken off every hit it takes; a hit always deals at least one point.
        public var armour: UInt32

        /// A building to place; no entity and no armour by default.
        public init(
            team: UInt32, entity: Entity = .null, position: FixedVec2, radius: Fixed,
            health: UInt32, armour: UInt32 = 0
        ) {
            self.team = team
            self.entity = entity
            self.position = position
            self.radius = radius
            self.health = health
            self.armour = armour
        }
    }

    /// What a unit attacks, or what an event names.
    public enum Target: Equatable, Hashable, Sendable {
        case none
        case unit(UInt32)
        case structure(UInt32)

        init(kind: UInt32, index: UInt32) {
            switch LockstepTargetKind(rawValue: kind) {
            case .unit: self = .unit(index)
            case .structure: self = .structure(index)
            default: self = .none
            }
        }
    }

    /// An armed unit's state, as the last tick left it.
    public struct Combatant: Equatable, Sendable {
        /// `arm` gave it a weapon.
        public var armed: Bool
        /// Its health is above zero.
        public var alive: Bool
        /// Its target was within its range on the last tick.
        public var engaged: Bool
        /// It is following a path to its target.
        public var chasing: Bool
        /// The side it fights for.
        public var team: UInt32
        /// What it has left.
        public var health: UInt32
        /// What it was armed with.
        public var maxHealth: UInt32
        /// Taken off every hit it takes.
        public var armour: UInt32
        /// Ticks until it may attack again.
        public var cooldown: UInt32
        /// Units and structures it finished.
        public var kills: UInt32
        /// What it is doing, as far as fighting goes.
        public var stance: LockstepStance
        /// What it is attacking, or `.none`.
        public var target: Target

        init(_ raw: CyLockstepCombatant) {
            armed = raw.flags & CY_LOCKSTEP_COMBATANT_ARMED != 0
            alive = raw.flags & CY_LOCKSTEP_COMBATANT_ALIVE != 0
            engaged = raw.flags & CY_LOCKSTEP_COMBATANT_ENGAGED != 0
            chasing = raw.flags & CY_LOCKSTEP_COMBATANT_CHASING != 0
            team = raw.team
            health = raw.health
            maxHealth = raw.max_health
            armour = raw.armour
            cooldown = raw.cooldown
            kills = raw.kills
            stance = LockstepStance(rawValue: raw.stance) ?? .idle
            target = Target(kind: raw.target_kind, index: raw.target)
        }
    }

    /// A structure's state.
    public struct Structure: Equatable, Sendable {
        /// The side it belongs to.
        public var team: UInt32
        /// The entity that draws it.
        public var entity: Entity
        /// Its centre: x and world Z.
        public var position: FixedVec2
        /// Its footprint.
        public var radius: Fixed
        /// What it has left: zero once it has fallen.
        public var health: UInt32
        /// What it was placed with.
        public var maxHealth: UInt32
        /// Its health is above zero.
        public var standing: Bool { health > 0 }
    }

    /// One thing a tick's combat did.
    public struct Event: Equatable, Sendable {
        /// From zero, in the order the session made them.
        public var sequence: UInt64
        /// The tick that made it.
        public var tick: UInt64
        /// What happened.
        public var kind: LockstepEventKind
        /// The unit that acted — or, for a death, died — or nil.
        public var unit: UInt32?
        /// What was struck, aimed at, damaged or destroyed.
        public var target: Target
        /// A hit's points, or the enemies an impact hit.
        public var amount: UInt32
        /// What a hit left its target.
        public var health: UInt32
        /// A launch's or an impact's projectile.
        public var projectile: UInt32
        /// The acting side's team; for a death or a fall, the fallen's.
        public var team: UInt32
        /// Where it happened: the striker, the muzzle, the landing, the struck, the fallen.
        public var position: FixedVec2
        /// A blow's or a launch's target position; an impact's origin.
        public var toward: FixedVec2

        init?(_ raw: CyLockstepEvent) {
            guard let kind = LockstepEventKind(rawValue: raw.kind) else { return nil }
            sequence = raw.sequence
            tick = raw.tick
            self.kind = kind
            unit = raw.unit == CY_LOCKSTEP_NO_UNIT ? nil : raw.unit
            target = Target(kind: raw.target_kind, index: raw.target)
            amount = raw.amount
            health = raw.health
            projectile = raw.projectile
            team = raw.team
            position = FixedVec2(raw.position)
            toward = FixedVec2(raw.toward)
        }
    }

    /// A projectile in flight.
    public struct Projectile: Equatable, Sendable {
        /// Its number, from zero in launch order: an impact names it.
        public var id: UInt32
        /// The unit that launched it.
        public var shooter: UInt32
        /// The shooter's side.
        public var team: UInt32
        /// What it flies at.
        public var target: Target
        /// The tick it left.
        public var launched: UInt64
        /// Where it left from.
        public var origin: FixedVec2
        /// Where it is now.
        public var position: FixedVec2
        /// Where it is going: its target, or where its target fell.
        public var aim: FixedVec2
    }

    /// Give unit `unit` health and a weapon, before the first tick.
    public static func arm(_ unit: UInt32, _ arms: Arms) throws {
        let engine = try GameServices.engine()
        var raw = CyLockstepCombatDesc()
        raw.struct_size = UInt32(MemoryLayout<CyLockstepCombatDesc>.size)
        raw.team = arms.team
        raw.health = arms.health
        raw.armour = arms.armour
        raw.weapon = arms.weapon.rawValue
        raw.damage = arms.damage
        raw.cooldown = arms.cooldown
        raw.range = arms.range.raw
        raw.sight = arms.sight.raw
        raw.projectile_speed = arms.projectileSpeed.raw
        raw.splash = arms.splash.raw
        try engine.lockstepArm(unit: unit, desc: &raw)
    }

    /// Place a structure, before the first tick. Returns its index.
    @discardableResult
    public static func fortify(_ desc: StructureDesc) throws -> UInt32 {
        let engine = try GameServices.engine()
        var raw = CyLockstepStructureDesc()
        raw.struct_size = UInt32(MemoryLayout<CyLockstepStructureDesc>.size)
        raw.team = desc.team
        raw.entity = desc.entity.bits
        raw.position = desc.position.abi
        raw.radius = desc.radius.raw
        raw.health = desc.health
        raw.armour = desc.armour
        var index: UInt32 = 0
        try engine.lockstepFortify(desc: &raw, into: &index)
        return index
    }

    /// Unit `unit`'s combat state.
    public static func combatant(_ unit: UInt32) throws -> Combatant {
        let engine = try GameServices.engine()
        var raw = CyLockstepCombatant()
        raw.struct_size = UInt32(MemoryLayout<CyLockstepCombatant>.size)
        try engine.lockstepCombatant(unit: unit, into: &raw)
        return Combatant(raw)
    }

    /// Structure `index`'s state.
    public static func structure(_ index: UInt32) throws -> Structure {
        let engine = try GameServices.engine()
        var raw = CyLockstepStructure()
        raw.struct_size = UInt32(MemoryLayout<CyLockstepStructure>.size)
        try engine.lockstepStructure(structure: index, into: &raw)
        return Structure(
            team: raw.team, entity: Entity(bits: raw.entity), position: FixedVec2(raw.position),
            radius: Fixed(raw: raw.radius), health: raw.health, maxHealth: raw.max_health)
    }

    /// The events numbered `first` and after, oldest first.
    public static func events(from first: UInt64) throws -> [Event] {
        let engine = try GameServices.engine()
        var count: UInt32 = 0
        try engine.lockstepEvents(first: first, into: nil, capacity: 0, count: &count)
        guard count > 0 else { return [] }
        var raw = [CyLockstepEvent](repeating: CyLockstepEvent(), count: Int(count))
        var written: UInt32 = 0
        try raw.withUnsafeMutableBufferPointer { buffer in
            try engine.lockstepEvents(
                first: first, into: buffer.baseAddress, capacity: count, count: &written)
        }
        return raw.prefix(Int(written)).compactMap(Event.init)
    }

    /// The projectiles in flight, in launch order.
    public static var projectiles: [Projectile] {
        get throws {
            let engine = try GameServices.engine()
            var count: UInt32 = 0
            try engine.lockstepProjectiles(into: nil, capacity: 0, count: &count)
            guard count > 0 else { return [] }
            var raw = [CyLockstepProjectile](repeating: CyLockstepProjectile(), count: Int(count))
            var written: UInt32 = 0
            try raw.withUnsafeMutableBufferPointer { buffer in
                try engine.lockstepProjectiles(
                    into: buffer.baseAddress, capacity: count, count: &written)
            }
            return raw.prefix(Int(written)).map {
                Projectile(
                    id: $0.id, shooter: $0.shooter, team: $0.team,
                    target: Target(kind: $0.target_kind, index: $0.target), launched: $0.launched,
                    origin: FixedVec2($0.origin), position: FixedVec2($0.position),
                    aim: FixedVec2($0.aim))
            }
        }
    }
}
