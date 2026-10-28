// SPDX-License-Identifier: MIT
// Battle.swift — a battle fought through the engine's lockstep path, and drawn where blue can see
// it. ABI 1.16 (combat), 1.15 (effects), 1.11 (lights), 1.10 (fog of war),
// openspec/changes/add-rts-combat-and-minimap.
//
// THE FIGHT IS BITS. On the company's field, blue's swordsmen, archers and catapults and red's
// swordsmen and archers, before a watchtower and a barracks, attack each other. The commander
// enlists and arms every unit and places red's two buildings in `onCreate`, and gives every order
// from a fixed step: both armies attack-move at each other on tick 0; on tick 540 blue's soldiers
// push on through what is left of red while its catapults turn on the watchtower; on tick 720 the
// soldiers join them, and on tick 960 all of blue goes for the barracks. Every number here — a soldier's
// place, its health, its weapon's reach and speed, an order's point — is an integer or a `Fixed`,
// so the session decides the same blows, flights, hits and deaths on every peer and every
// architecture. host/battle.h states the same battle in C++; `integration.rts_api_sample` holds
// the digest this Swift drives to the one every CI leg computes from that C++.
//
// WHAT IT LOOKS LIKE IS PRESENTATION. Every frame the commander reads what the session's ticks did
// (`Lockstep.events(from:)`, a cursor kept so each event is read once) and, where BLUE'S FOG OF
// WAR lets blue see the place, plays it: a launch's puff and a bowstring or a catapult's thump, a
// blow's sparks and clash, an arrow's sparks bouncing off the ground, a catapult shot's or a
// falling building's explosion with a short-lived light, a boom and smoke, a death's fall. Every
// projectile in flight leaves a trail where blue sees it, and a building the battle has damaged
// smokes. Nothing played feeds back: a resimulated tick would not change a single hit.

import CyberdyneKit

struct Battle {
    // --- The battle, as host/battle.h states it ----------------------------------------------

    /// What each kind of soldier is. `battle::Kind`, case for case.
    enum Kind: UInt32 {
        case blueSword = 0
        case blueArcher
        case catapult
        case redSword
        case redArcher

        var team: UInt32 { self == .redSword || self == .redArcher ? Battle.red : Battle.blue }
    }

    /// A squad: its kind, its group, how many, and its block — first unit's place, units a row,
    /// spacing — in quarter metres. `battle::Squad`.
    struct Squad {
        let kind: Kind
        let group: UInt32
        let count: UInt32
        let x: Int32
        let z: Int32
        let perRow: UInt32
        let spacing: Int32
    }

    /// A building: centre, radius in quarter metres, health and armour. `battle::Building`.
    struct Building {
        let x: Int32
        let z: Int32
        let radius: Int32
        let health: UInt32
        let armour: UInt32
    }

    /// One order of the plan. `battle::PlannedOrder`.
    struct Planned {
        let tick: UInt64
        let attack: Bool
        let group: UInt32
        let x: Int32
        let z: Int32
    }

    static let blue: UInt32 = 1
    static let red: UInt32 = 2
    static let squads: [Squad] = [
        Squad(kind: .blueSword, group: 0, count: 10, x: 12 * 4, z: 4 * 4, perRow: 5, spacing: 9),
        Squad(kind: .blueArcher, group: 1, count: 10, x: 5 * 4, z: 4 * 4, perRow: 4, spacing: 9),
        Squad(kind: .catapult, group: 2, count: 4, x: 3 * 4, z: 13 * 4, perRow: 4, spacing: 12),
        Squad(kind: .redSword, group: 3, count: 8, x: 30 * 4, z: 5 * 4, perRow: 4, spacing: 9),
        Squad(kind: .redArcher, group: 4, count: 8, x: 37 * 4, z: 10 * 4, perRow: 4, spacing: 9),
    ]
    static let buildings: [Building] = [
        Building(x: 44 * 4, z: 4 * 4, radius: 10, health: 400, armour: 3),
        Building(x: 42 * 4, z: 16 * 4, radius: 12, health: 400, armour: 4),
    ]
    static let plan: [Planned] = [
        Planned(tick: 0, attack: false, group: 0, x: 34 * 4, z: 7 * 4),
        Planned(tick: 0, attack: false, group: 1, x: 26 * 4, z: 7 * 4),
        Planned(tick: 0, attack: false, group: 2, x: 16 * 4, z: 10 * 4),
        Planned(tick: 0, attack: false, group: 3, x: 14 * 4, z: 6 * 4),
        Planned(tick: 0, attack: false, group: 4, x: 22 * 4, z: 9 * 4),
        Planned(tick: 540, attack: false, group: 0, x: 40 * 4, z: 9 * 4),
        Planned(tick: 540, attack: false, group: 1, x: 36 * 4, z: 9 * 4),
        Planned(tick: 540, attack: true, group: 2, x: 44 * 4, z: 4 * 4),
        Planned(tick: 720, attack: true, group: 0, x: 44 * 4, z: 4 * 4),
        Planned(tick: 720, attack: true, group: 1, x: 44 * 4, z: 4 * 4),
        Planned(tick: 960, attack: true, group: 0, x: 42 * 4, z: 16 * 4),
        Planned(tick: 960, attack: true, group: 1, x: 42 * 4, z: 16 * 4),
        Planned(tick: 960, attack: true, group: 2, x: 42 * 4, z: 16 * 4),
    ]

    /// `q` quarter metres, exactly.
    static func quarters(_ q: Int32) -> Fixed {
        Fixed(raw: Int64(q) * (Fixed.oneRaw / 4))
    }

    /// Where unit `index` of `squad` starts. `battle::spawn`.
    static func spawn(_ squad: Squad, _ index: UInt32) -> FixedVec2 {
        let column = Int32(index % squad.perRow)
        let row = Int32(index / squad.perRow)
        return FixedVec2(
            x: quarters(squad.x + column * squad.spacing),
            y: quarters(squad.z + row * squad.spacing))
    }

    /// A kind's weapon and health. `battle::arms`.
    static func arms(_ kind: Kind) -> Lockstep.Arms {
        switch kind {
        case .blueSword, .redSword:
            let blueSide = kind == .blueSword
            return Lockstep.Arms(
                team: kind.team, health: blueSide ? 120 : 130, armour: blueSide ? 3 : 2,
                weapon: .melee, damage: blueSide ? 16 : 18, cooldown: 36,
                range: quarters(5), sight: Fixed(8))
        case .blueArcher, .redArcher:
            let blueSide = kind == .blueArcher
            return Lockstep.Arms(
                team: kind.team, health: blueSide ? 70 : 75, weapon: .ranged,
                damage: blueSide ? 11 : 13, cooldown: 54, range: Fixed(blueSide ? 11 : 10),
                sight: Fixed(13), projectileSpeed: Fixed(22))
        case .catapult:
            return Lockstep.Arms(
                team: kind.team, health: 150, armour: 2, weapon: .ranged, damage: 30,
                cooldown: 150, range: Fixed(20), sight: Fixed(21), projectileSpeed: Fixed(15),
                splash: quarters(10))
        }
    }

    // --- Presentation: the field's place on the map, and what each side sees -----------------

    /// Where the field lies in the level's metres (`battle::kFieldX`, `kFieldZ`).
    static let origin = Vec3(x: -8, y: 0, z: -52)
    static let originFixed = FixedVec2(x: Fixed(-8), y: Fixed(-52))
    /// How far each kind sees for the fog of war, in metres.
    static func vision(_ kind: Kind) -> Fixed {
        switch kind {
        case .blueSword, .redSword: return Fixed(8)
        case .blueArcher, .redArcher: return Fixed(12)
        case .catapult: return Fixed(10)
        }
    }

    /// A field position in the level's metres, at `height`.
    static func level(_ at: FixedVec2, height: Float) -> Vec3 {
        Vec3(
            x: origin.x + Float(at.x.presentation), y: height,
            z: origin.z + Float(at.y.presentation))
    }

    // --- State -----------------------------------------------------------------------------

    /// The units, in enlistment order, their kinds, and the soldier each is drawn as (null once
    /// the game has taken a fallen one off the field).
    private(set) var units: [UInt32] = []
    private var kinds: [Kind] = []
    private var soldiers: [Entity] = []
    private(set) var structures: [UInt32] = []
    /// The first event not yet read.
    private var cursor: UInt64 = 0
    private var frames: UInt64 = 0
    private var look = BattleLook()
    private(set) var tally = BattleTally()

    /// `onCreate`: a soldier spawned for every unit — the node the fog of war places its vision
    /// by — enlisted, armed and given its side's sight; then red's buildings.
    mutating func create(soldier prefab: Prefab) throws {
        for squad in Battle.squads {
            for index in 0..<squad.count {
                let at = Battle.spawn(squad, index)
                let soldier = try prefab.instantiate(
                    at: Pose(position: Battle.level(at, height: 0)))
                let unit = try Lockstep.enlist(
                    .init(
                        group: squad.group, entity: soldier, position: at,
                        radius: squad.kind == .catapult ? Battle.quarters(4) : .zero,
                        maxSpeed: squad.kind == .catapult ? Battle.quarters(12) : .zero))
                try Lockstep.arm(unit, Battle.arms(squad.kind))
                try FogOfWar.setVision(
                    .init(team: squad.kind.team, revealRadius: Battle.vision(squad.kind)),
                    of: soldier)
                units.append(unit)
                kinds.append(squad.kind)
                soldiers.append(soldier)
            }
        }
        for building in Battle.buildings {
            structures.append(
                try Lockstep.fortify(
                    .init(
                        team: Battle.red,
                        position: FixedVec2(
                            x: Battle.quarters(building.x), y: Battle.quarters(building.z)),
                        radius: Battle.quarters(building.radius), health: building.health,
                        armour: building.armour)))
        }
        look = BattleLook.resolve()
        tally.soldiers = Float(units.count)
    }

    /// A fixed step, before the session runs its tick: the plan's orders for it, and the fallen
    /// taken off the field so they no longer see.
    mutating func step() throws {
        let tick = try Lockstep.status.tick
        for planned in Battle.plan where planned.tick == tick {
            let point = FixedVec2(x: Battle.quarters(planned.x), y: Battle.quarters(planned.z))
            try Lockstep.order(
                planned.attack
                    ? .attack(group: planned.group, at: point)
                    : .attackMove(group: planned.group, to: point))
            tally.orders += 1
        }
        for (index, unit) in units.enumerated() where !soldiers[index].isNull {
            if !(try Lockstep.combatant(unit).alive) {
                try Spawn.destroy(soldiers[index])
                soldiers[index] = .null
            }
        }
    }

    /// `onUpdate`: everything the ticks since the last frame did, played where blue sees it, and
    /// the projectiles' trails.
    mutating func frame() throws {
        frames += 1
        for event in try Lockstep.events(from: cursor) {
            cursor = event.sequence + 1
            tally.events += 1
            try present(event)
        }
        for shot in try Lockstep.projectiles {
            try trail(shot)
        }
        if frames % 15 == 0 {
            try smokeDamagedBuildings()
        }
    }

    /// Where blue's living army stands, in the level's metres: what the camera follows.
    func front() throws -> Vec3? {
        var sum = Vec3()
        var count: Float = 0
        for (index, unit) in units.enumerated() where kinds[index].team == Battle.blue {
            guard try Lockstep.combatant(unit).alive else { continue }
            let at = Battle.level(try Lockstep.unit(unit).position, height: 0)
            sum = Vec3(x: sum.x + at.x, y: 0, z: sum.z + at.z)
            count += 1
        }
        return count > 0 ? Vec3(x: sum.x / count, y: 0, z: sum.z / count) : nil
    }

    // --- Presentation ----------------------------------------------------------------------

    private func shown(_ at: FixedVec2) throws -> Bool {
        let world = FixedVec2(x: Battle.originFixed.x + at.x, y: Battle.originFixed.y + at.y)
        return try FogOfWar.isVisible(team: Battle.blue, at: world)
    }

    private func kind(of unit: UInt32?) -> Kind? {
        guard let unit, let index = units.firstIndex(of: unit) else { return nil }
        return kinds[index]
    }

    private mutating func present(_ event: Lockstep.Event) throws {
        switch event.kind {
        case .damage:
            tally.damage += Float(event.amount)
            return
        case .death:
            tally.deaths += 1
        case .destroyed:
            tally.fallen += 1
        case .strike, .launch, .impact:
            break
        }
        guard try shown(event.position) else {
            tally.hidden += 1
            return
        }
        tally.shown += 1
        let at = Battle.level(event.position, height: 0)
        switch event.kind {
        case .strike:
            let target = Battle.level(event.toward, height: 1.1)
            let normal = direction(from: target, to: Battle.level(event.position, height: 1.1))
            try play(look.sparks, at: target, parameters: sparks(normal, over: target))
            try sound(look.clash, at: at)
        case .launch:
            let heavy = kind(of: event.unit) == .catapult
            let from = Battle.level(event.position, height: heavy ? 1.6 : 1.4)
            let aim = direction(from: from, to: Battle.level(event.toward, height: 1.4))
            try play(
                look.launch, at: from, scale: heavy ? 2.2 : 1, parameters: [aim.x, aim.y, aim.z])
            try sound(heavy ? look.thump : look.bow, at: from)
        case .impact:
            if kind(of: event.unit) == .catapult {
                try blast(at: at, scale: 1)
            } else {
                let hit = Battle.level(event.position, height: 1.0)
                let back = direction(from: hit, to: Battle.level(event.toward, height: 1.6))
                try play(look.sparks, at: hit, parameters: sparks(back, over: hit))
            }
        case .death:
            try play(look.smoke, at: at, scale: 0.4)
            try sound(look.fall, at: at)
        case .destroyed:
            try blast(at: Battle.level(event.position, height: 1.5), scale: 2.6)
            try play(look.smoke, at: Battle.level(event.position, height: 3), scale: 2.2)
        case .damage:
            break
        }
    }

    /// An explosion: the fireball and embers bouncing off the ground, a flash of light that dies in
    /// half a second, smoke, and a boom.
    private mutating func blast(at: Vec3, scale: Float) throws {
        // The ground is the `floor` parameter, in the effect's own space: the blast's height below.
        try play(look.explosion, at: at, scale: scale, parameters: [-at.y])
        try play(look.smoke, at: at, scale: scale)
        do {
            _ = try Light.spawn(
                .point(
                    at: Vec3(x: at.x, y: at.y + 1.5 * scale, z: at.z),
                    colour: Vec3(x: 1, y: 0.56, z: 0.22), candela: 120_000 * scale * scale,
                    range: 14 * scale, lifetime: 0.25 + 0.25 * scale, fade: .quadratic))
            tally.lights += 1
        } catch CyberdyneError.status(.unavailable, _) {
            // A host that draws nothing lends no lights.
        } catch CyberdyneError.status(.outOfRange, _) {
            // Every slot of the pool is flashing already.
        }
        try sound(look.boom, at: at)
    }

    /// One frame of a projectile's trail, on its arc: an arrow's low, a catapult shot's high.
    private mutating func trail(_ shot: Lockstep.Projectile) throws {
        guard try shown(shot.position) else { return }
        let heavy = kind(of: shot.shooter) == .catapult
        let total = distance(shot.origin, shot.aim)
        let gone = distance(shot.origin, shot.position)
        let t = total > 0 ? min(max(gone / total, 0), 1) : 1
        let apex: Float = heavy ? 6 : 1.2
        let height = 1.4 + 4 * apex * t * (1 - t)
        try play(look.trail, at: Battle.level(shot.position, height: height), scale: heavy ? 2 : 1)
        tally.trails += 1
    }

    private mutating func smokeDamagedBuildings() throws {
        for structure in structures {
            let state = try Lockstep.structure(structure)
            guard state.health * 2 < state.maxHealth, try shown(state.position) else { continue }
            let height: Float = state.standing ? 5 : 1.5
            try play(look.smoke, at: Battle.level(state.position, height: height), scale: 1.4)
        }
    }

    private mutating func play(
        _ effect: Effect?, at position: Vec3, scale: Float = 1, parameters: [Float] = []
    ) throws {
        guard let effect else { return }
        do {
            try effect.play(at: position, scale: scale, parameters: parameters)
            tally.effects += 1
        } catch CyberdyneError.status(.outOfRange, _) {
            // The simulation world is full this frame: the effect is dropped, as a game's are.
        }
    }

    private mutating func sound(_ cue: AudioCue?, at position: Vec3) throws {
        guard let cue else { return }
        if try Audio.play(cue, at: position) != nil {
            tally.sounds += 1
        }
    }

    /// The sparks' parameters: the struck surface's normal (four floats, the fourth unused) and
    /// the ground's height under the hit, in the effect's own space.
    private func sparks(_ normal: Vec3, over hit: Vec3) -> [Float] {
        [normal.x, normal.y, normal.z, 0, -hit.y]
    }

    private func direction(from: Vec3, to: Vec3) -> Vec3 {
        let d = Vec3(x: to.x - from.x, y: to.y - from.y, z: to.z - from.z)
        let length = (d.x * d.x + d.y * d.y + d.z * d.z).squareRoot()
        return length > 1e-4
            ? Vec3(x: d.x / length, y: d.y / length, z: d.z / length) : Vec3(x: 0, y: 1, z: 0)
    }

    private func distance(_ a: FixedVec2, _ b: FixedVec2) -> Float {
        let dx = Float(a.x.presentation - b.x.presentation)
        let dz = Float(a.y.presentation - b.y.presentation)
        return (dx * dx + dz * dz).squareRoot()
    }
}

/// The battle's effects and cues, resolved by name once. A host that draws nothing registers no
/// effects, and the battle is fought and counted all the same.
struct BattleLook {
    var launch: Effect?
    var trail: Effect?
    var sparks: Effect?
    var explosion: Effect?
    var smoke: Effect?
    var clash: AudioCue?
    var bow: AudioCue?
    var thump: AudioCue?
    var boom: AudioCue?
    var fall: AudioCue?

    static func resolve() -> BattleLook {
        BattleLook(
            launch: try? Effect.named(Content.launchEffect),
            trail: try? Effect.named(Content.trailEffect),
            sparks: try? Effect.named(Content.sparksEffect),
            explosion: try? Effect.named(Content.explosionEffect),
            smoke: try? Effect.named(Content.smokeEffect),
            clash: try? Audio.cue(Content.clashCue), bow: try? Audio.cue(Content.bowCue),
            thump: try? Audio.cue(Content.thumpCue), boom: try? Audio.cue(Content.boomCue),
            fall: try? Audio.cue(Content.fallCue))
    }
}

/// What the battle did, for `RtsReport`.
struct BattleTally {
    var soldiers: Float = 0
    var orders: Float = 0
    var events: Float = 0
    var damage: Float = 0
    var deaths: Float = 0
    var fallen: Float = 0
    /// Events blue saw and was shown, and events in the fog it was not.
    var shown: Float = 0
    var hidden: Float = 0
    var effects: Float = 0
    var lights: Float = 0
    var sounds: Float = 0
    var trails: Float = 0
}
