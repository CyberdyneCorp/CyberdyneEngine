// SPDX-License-Identifier: MIT
// Skirmish.swift — the battle as a game a person plays: blue's army under the player's hand, red's
// under an opponent's, until one side has won.
//
// The host places `/Level/Skirmish` beside `/Level/Battlefield` when it runs a player's session
// (`--play`); the commander then enlists the battle's two armies with a group a unit
// (`Battle.Mode.skirmish`) and hands the frame and the fixed step to this type instead of the
// plan. Everything a person does arrives as input — keys through the host's actions, the pointer
// through `Input.pointer` — and everything it decides is decided here:
//
//   camera      SkirmishCamera.swift: pan, zoom, turn, and a jump from the minimap
//   selection   a click on a unit, a box dragged round several, shift to add, E for the whole
//               army; the selected units outlined (`World.highlight`), an enemy under the pointer
//               glowing
//   groups      Ctrl and 1 to 9 keeps the selection; the number calls it back, twice brings the
//               camera to it
//   orders      right click: on an enemy unit or building, attack it; on the ground, move there —
//               or attack-move with Ctrl held, or after F (or the Attack button); H stops
//   opponent    RedOpponent.swift, from the fixed step, through the same orders
//   the end     Victory when both of red's buildings have fallen, defeat when no blue unit is
//               left; the result screen; P pauses, Esc opens the menu
//
// The session itself stops for a pause: the game asks the host through `SkirmishControl`, the one
// component the host reads to act on, and the host runs frames without fixed steps until it is
// asked to go on. In a replay (`/Level/SkirmishReplay`, `--replay <log>`) the host gives every
// order from the recorded log, and this type gives none — not the player's and not the opponent's
// — and only watches.

import CyberdyneKit

/// What the game asks of the host running it. Written every frame; the host reads it after the
/// frame and acts on it — the one component that goes that way. `paused` stops the fixed steps
/// (frames still run); `quit` ends the session after the frame.
@Component
struct SkirmishControl {
    var paused: Float = 0
    var quit: Float = 0
}

final class Skirmish {
    /// Whether the player may give orders: false in a replay, where the log gives them all.
    let commanding: Bool

    private var camera: SkirmishCamera
    private var selection = SkirmishSelection()
    private var groups = ControlGroups()
    private var opponent = RedOpponent()
    private var pending: [PlayerOrder] = []
    private let intents = SkirmishIntents()
    private var ui: SkirmishUI?
    /// The units as this frame read them.
    private var troops: [Trooper] = []
    /// F (or the Attack button) was pressed: the next left click on the ground is an attack-move.
    private var armed = false
    private var paused = false
    private var menuOpen = false
    private var quit = false
    private var outcome = Outcome.playing
    private var watching = false
    private var clock: Double = 0
    private var hovered: Entity = .null
    /// The pointer has moved since the session began: until it has, its place is nobody's.
    private var pointerMoved = false
    private var outlined: Set<Entity> = []
    private var controls = ComponentType(id: 0)
    private(set) var tally = SkirmishTally()

    init(owner: Behaviour, battle: Battle, commanding: Bool) throws {
        self.commanding = commanding
        // Over blue's army, looking east at red's: a quarter turn from looking down world -z.
        let start = Battle.level(
            FixedVec2(x: Battle.quarters(14 * 4), y: Battle.quarters(9 * 4)), height: 0)
        camera = SkirmishCamera(focus: start, heading: Angle(raw: 0xC000_0000), distance: 38)
        do {
            ui = try SkirmishUI(for: owner, intents: intents)
        } catch CyberdyneError.status(.unavailable, _) {
            ui = nil
        }
        if let world = owner.world {
            controls = try Components.register(SkirmishControl.self, in: world)
            if !world.has(controls, on: owner.entity) {
                try world.add(controls, to: owner.entity)
            }
        }
        troops = try SkirmishArmy.read(battle)
        tally.camera = camera
    }

    // --- The frame -----------------------------------------------------------------------------

    /// `onUpdate`: the player's hand read, the camera flown, units picked, orders recorded, the
    /// interface written, and what the host is asked to do.
    func frame(_ delta: Double, battle: Battle, hud: RtsHud?, owner: Behaviour) throws {
        clock += delta
        guard let active = try Camera.active() else { return }
        troops = try SkirmishArmy.read(battle)
        try SkirmishArmy.project(&troops, through: active)
        selection.prune(troops)

        let pointer = try Input.pointer()
        let modifiers = try Input.modifiers()
        try readKeys(modifiers)
        actOnIntents()
        let overUI = try UI.hitTest(pointer.position) != nil || ui?.presented != nil
        if !overUI || selection.anchor != nil {
            try hand(pointer, modifiers: modifiers, through: active)
        } else if pointer.pressed.contains(.left), try minimap(pointer, hud: hud) {
            tally.minimapJumps += 1
        }
        pointerMoved = pointerMoved || pointer.delta.x != 0 || pointer.delta.y != 0
        if ui?.presented == nil {
            camera.steer(
                keys: try Input.action(Content.pan).axis2, pointer: pointer, edges: pointerMoved,
                viewport: try active.view().viewport, delta: Float(delta))
        }
        try camera.show(active)
        tally.camera = camera

        try outline(owner.world, pointer: pointer, overUI: overUI)
        try showScreen(hud: hud, battle: battle)
        try publish(owner)
        intents.clear()
    }

    /// `onFixedUpdate`: the orders the frames recorded, the opponent's thoughts, and whether
    /// anyone has won.
    func step(battle: inout Battle) throws {
        let tick = try Lockstep.status.tick
        troops = try SkirmishArmy.read(battle)
        if commanding {
            for order in pending {
                for command in order.commands(troops) {
                    try Lockstep.order(command)
                    battle.countOrder()
                    tally.count(order.kind)
                }
            }
            for _ in try opponent.step(tick: tick, troops: troops, structures: battle.structures) {
                battle.countOrder()
            }
            tally.opponentOrders = opponent.orders
            tally.posture = Float(opponent.posture.rawValue)
        }
        pending.removeAll()
        if outcome == .playing {
            outcome = try Outcome.judge(troops, structures: battle.structures)
            if outcome != .playing {
                tally.outcome = Float(outcome.rawValue)
                tally.outcomeTick = Float(tick)
            }
        }
    }

    // --- The keys --------------------------------------------------------------------------------

    private func readKeys(_ modifiers: Modifiers) throws {
        if try Input.action(SkirmishKeys.menu).justPressed {
            intents.menu = true
        }
        if try Input.action(SkirmishKeys.pause).justPressed {
            paused.toggle()
            tally.pauses += 1
        }
        guard ui?.presented == nil else { return }
        if try Input.action(SkirmishKeys.attack).justPressed {
            intents.attackMove = true
        }
        if try Input.action(SkirmishKeys.stop).justPressed {
            intents.stop = true
        }
        if try Input.action(SkirmishKeys.army).justPressed {
            intents.army = true
        }
        for group in 0..<9 where try Input.action(SkirmishKeys.group(group)).justPressed {
            if modifiers.contains(.ctrl) {
                groups.assign(group, selection.chosen)
            } else {
                let called = groups.recall(group, troops: troops, now: clock)
                selection.set(called.units)
                if called.twice, let centre = centre(of: called.units) {
                    camera.jump(to: centre)
                }
            }
        }
    }

    /// The buttons' intents and the keys', alike.
    private func actOnIntents() {
        if intents.menu {
            menu(open: ui?.presented != "menu-dialog")
        }
        if intents.resume {
            menu(open: false)
        }
        if intents.watch {
            watching = true
        }
        if intents.quit {
            quit = true
        }
        guard commanding else { return }
        if intents.attackMove && !selection.chosen.isEmpty {
            armed = true
        }
        if intents.stop && !selection.chosen.isEmpty {
            pending.append(PlayerOrder(kind: .stop, units: selection.chosen, point: FixedVec2()))
        }
        if intents.army {
            selection.set(troops.filter { $0.isBlue && $0.alive }.map(\.index))
        }
    }

    private func menu(open: Bool) {
        if open && !menuOpen {
            tally.menus += 1
        }
        menuOpen = open
    }

    // --- The pointer -----------------------------------------------------------------------------

    /// Left: press, drag, release — a pick or a box, or with an attack-move armed, its point.
    /// Right: an order.
    private func hand(_ pointer: Pointer, modifiers: Modifiers, through camera: Camera) throws {
        let at = pointer.position
        if pointer.pressed.contains(.left) {
            if armed, let ground = try groundPoint(at, camera) {
                give(.attackMove, at: ground)
                armed = false
                return
            }
            selection.press(at: at)
        }
        if pointer.buttons.contains(.left) {
            selection.drag(to: at)
        }
        if pointer.released.contains(.left) && selection.anchor != nil {
            selection.release(at: at, troops: troops, adding: modifiers.contains(.shift))
        }
        if pointer.pressed.contains(.right) {
            armed = false
            try order(at: at, attackMove: modifiers.contains(.ctrl), camera)
        }
    }

    /// A right click: attack what is under it, or go there.
    private func order(at point: Vec2, attackMove: Bool, _ camera: Camera) throws {
        guard commanding, !selection.chosen.isEmpty else { return }
        if let enemy = SkirmishSelection.under(point, troops, team: Battle.red) {
            give(.attack, at: enemy.field)
            return
        }
        guard let ground = try groundPoint(point, camera) else { return }
        if let building = try buildingNear(ground) {
            give(.attack, at: building)
            return
        }
        give(attackMove ? .attackMove : .move, at: ground)
    }

    private func give(_ kind: PlayerOrder.Kind, at point: FixedVec2) {
        guard commanding, !selection.chosen.isEmpty else { return }
        pending.append(PlayerOrder(kind: kind, units: selection.chosen, point: point))
    }

    /// Where the pointer's ray meets the ground, on the field.
    private func groundPoint(_ point: Vec2, _ camera: Camera) throws -> FixedVec2? {
        let ray = try camera.ray(through: point)
        guard ray.direction.y < -1e-4 else { return nil }
        let along = -ray.origin.y / ray.direction.y
        let hit = Vec3(
            x: ray.origin.x + ray.direction.x * along, y: 0,
            z: ray.origin.z + ray.direction.z * along)
        return SkirmishArmy.field(hit)
    }

    /// A standing red building within two metres of its footprint of `ground`.
    private func buildingNear(_ ground: FixedVec2) throws -> FixedVec2? {
        for index in 0..<Battle.buildings.count {
            let building = Battle.buildings[index]
            let at = FixedVec2(x: Battle.quarters(building.x), y: Battle.quarters(building.z))
            let reach = Battle.quarters(building.radius + 8)
            if SkirmishArmy.distanceSquared(at, ground) < reach * reach {
                return at
            }
        }
        return nil
    }

    /// A left click on the minimap moves the camera there.
    private func minimap(_ pointer: Pointer, hud: RtsHud?) throws -> Bool {
        guard let hud else { return false }
        let rect = try hud.tree["minimap"].rect
        guard rect.width > 0, rect.contains(pointer.position) else { return false }
        let u = (pointer.position.x - rect.x) / rect.width
        let v = (pointer.position.y - rect.y) / rect.height
        let frame = MinimapFrame.frontier
        camera.jump(to: Vec3(x: frame.minX + u * frame.size, y: 0, z: frame.minZ + v * frame.size))
        return true
    }

    private func centre(of units: [Int]) -> Vec3? {
        guard !units.isEmpty else { return nil }
        var sum = Vec3()
        for index in units {
            sum = Vec3(x: sum.x + troops[index].at.x, y: 0, z: sum.z + troops[index].at.z)
        }
        let count = Float(units.count)
        return Vec3(x: sum.x / count, y: 0, z: sum.z / count)
    }

    // --- What the player sees ------------------------------------------------------------------

    /// The selected units outlined blue, the enemy under the pointer glowing red.
    private func outline(_ world: World?, pointer: Pointer, overUI: Bool) throws {
        guard let world else { return }
        var wanted: Set<Entity> = []
        for index in selection.chosen where !troops[index].soldier.isNull {
            wanted.insert(troops[index].soldier)
        }
        for entity in outlined.subtracting(wanted) where world.isAlive(entity) {
            try world.clearHighlight(entity)
        }
        for entity in wanted.subtracting(outlined) {
            try world.highlight(entity, .selected(red: 64, green: 160, blue: 255))
        }
        outlined = wanted
        let enemy =
            overUI ? nil : SkirmishSelection.under(pointer.position, troops, team: Battle.red)
        let now = enemy?.soldier ?? .null
        guard now != hovered else { return }
        if !hovered.isNull && world.isAlive(hovered) {
            try world.clearHighlight(hovered)
        }
        if !now.isNull {
            try world.highlight(now, .hovered(red: 255, green: 96, blue: 64))
        }
        hovered = now
    }

    private func showScreen(hud: RtsHud?, battle: Battle) throws {
        if outcome != .playing && !watching {
            let won = outcome == .victory
            try ui?.present(won ? "victory" : "defeat", summary: summary(battle))
        } else {
            try ui?.present(menuOpen ? "menu-dialog" : nil)
        }
        var screen = SkirmishScreen()
        screen.box = selection.box
        screen.armed = armed
        screen.status = try status()
        if outcome != .playing {
            screen.banner = outcome == .victory ? "Victory" : "Defeat"
        } else if !commanding {
            screen.banner = "Replay"
        } else if armed {
            screen.banner = "Attack-move: click a point"
        } else if isPaused {
            screen.banner = "Paused (P)"
        }
        try ui?.show(screen)
        try hud?.show(model(battle))
    }

    /// The armies' strength and the battle's time — the session's ticks, so a pause stops it.
    private func status() throws -> String {
        let blue = troops.filter { $0.isBlue && $0.alive }.count
        let red = troops.filter { !$0.isBlue && $0.alive }.count
        let seconds = Int(try Lockstep.status.tick / 60)
        let time = "\(seconds / 60):\(seconds % 60 < 10 ? "0" : "")\(seconds % 60)"
        return "Blue \(blue)  Red \(red)  \(time)"
    }

    private func summary(_ battle: Battle) -> String {
        let lost = troops.filter { $0.isBlue && !$0.alive }.count
        let killed = troops.filter { !$0.isBlue && !$0.alive }.count
        return "Lost \(lost), killed \(killed), \(Int(battle.tally.fallen)) buildings fell"
    }

    /// The HUD's model: the selection's rows, and the camera's frame on the minimap.
    private func model(_ battle: Battle) -> HudModel {
        var model = HudModel()
        let blue = troops.filter { $0.isBlue }
        model.resources = .init(
            gold: UInt32(troops.filter { !$0.isBlue && !$0.alive }.count * 25), wood: 0,
            food: UInt32(blue.filter(\.alive).count), foodCap: UInt32(blue.count))
        model.units = selection.chosen.map {
            .init(
                name: troops[$0].name, health: troops[$0].health,
                maxHealth: troops[$0].maxHealth)
        }
        let frame = MinimapFrame.frontier
        let seen = camera.footprint
        model.camera = UIRect(
            x: frame.x(seen.minX), y: frame.y(seen.minZ),
            width: (seen.maxX - seen.minX) / frame.size,
            height: (seen.maxZ - seen.minZ) / frame.size)
        model.fogPage = Content.fogPage
        return model
    }

    private var isPaused: Bool { paused || menuOpen || (outcome != .playing && !watching) }

    private func publish(_ owner: Behaviour) throws {
        tally.selected = Float(selection.chosen.count)
        tally.selections = selection.changes
        tally.assigned = groups.assigned
        tally.recalled = groups.recalled
        if isPaused {
            tally.pausedFrames += 1
        }
        guard let world = owner.world else { return }
        try world.setFloat(isPaused ? 1 : 0, owner.entity, controls, field: 0)
        try world.setFloat(quit ? 1 : 0, owner.entity, controls, field: 1)
    }
}

/// The keys a skirmish reads, as the host declares them (host/level.cpp).
enum SkirmishKeys {
    static let menu = "skirmish.menu"
    static let pause = "skirmish.pause"
    static let attack = "skirmish.attack"
    static let stop = "skirmish.stop"
    static let army = "skirmish.army"
    static func group(_ index: Int) -> String { "skirmish.group\(index + 1)" }
}

/// What the skirmish did, for `RtsReport`.
struct SkirmishTally {
    var moves: Float = 0
    var attackMoves: Float = 0
    var attacks: Float = 0
    var stops: Float = 0
    var opponentOrders: Float = 0
    var posture: Float = 0
    var selected: Float = 0
    var selections: Float = 0
    var assigned: Float = 0
    var recalled: Float = 0
    var minimapJumps: Float = 0
    var menus: Float = 0
    var pauses: Float = 0
    var pausedFrames: Float = 0
    var outcome: Float = 0
    var outcomeTick: Float = 0
    var camera = SkirmishCamera(focus: Vec3(), heading: Angle(raw: 0), distance: 30)

    mutating func count(_ kind: PlayerOrder.Kind) {
        switch kind {
        case .move: moves += 1
        case .attackMove: attackMoves += 1
        case .attack: attacks += 1
        case .stop: stops += 1
        }
    }
}
