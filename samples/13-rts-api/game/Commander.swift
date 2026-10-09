// SPDX-License-Identifier: MIT
// Commander.swift — the player's side of an RTS: a squad, a selection, move orders, a build key and
// a camera. All of it goes through ABI 1.3 by way of CyberdyneKit.
//
// WHICH CALLBACK DOES WHAT, AND WHY. The engine calls this behaviour in three phases, and each game
// service says which phases may call it (openspec/specs/native-abi/spec.md):
//
//   onCreate       (no phase)    resolve the prefab and the cue, spawn the starting squad
//   onEnterTree,   (no phase,    the commander is a NODE (`/Level/Commander`), so the scene tree's
//   onReady         the pump)    pump delivers these; `@Node("../Barracks")` is resolved first
//   onUpdate       (frame)       read the pointer, pan the camera, pick a unit or a ground point,
//                                show the game on the HUD
//   onUIEvent      (frame)       the HUD's Build button was clicked (ABI 1.6), before `onUpdate`
//   onFixedUpdate  (fixed step)  hand the recorded order to navigation, hear arrivals, build,
//                                play each unit's animation from what its agent is doing, and
//                                order the lockstep company (ABI 1.8, Company.swift)
//
// ABI 1.7: EVERY UNIT ANIMATES FROM HERE. A unit is given an animator over the host's `worker` rig
// when it is enlisted. Each fixed step the game plays `walk` while its agent follows a path and
// `idle` when it stands, crossfading between them; an arrival plays the one-shot `cheer`, and the
// cheer's own `cheer_done` event — read in `onUpdate`, where the frame's events are — sends the
// unit back to `idle` on the next fixed step. The engine runs the animators; this file only asks.
//
// The pointer and the camera are device and presentation state, so a fixed step may not read them.
// Spawning changes the simulation, so a frame may not do it. A click is therefore RECORDED in
// `onUpdate` and ACTED ON in the next `onFixedUpdate`, which is the pattern design.md gives for an
// RTS. Everything the fixed step reads (action state, navigation state) replays the same way.
//
// THE HUD IS PRESENTATION TOO. It is mounted in `onCreate`, written in `onUpdate` from the game's
// state, and its Build button's click — delivered in the frame — is recorded and acted on in the
// next fixed step, exactly as a key is. A left click that lands on the HUD is the HUD's, not a
// selection: `UI.hitTest` says which. With no interface bound (`--no-ui`) there is no HUD and
// every click is the world's.

import CyberdyneKit

@Behaviour(name: "Commander", schema: 1)
final class Commander: Behaviour {

    // --- Tunables ------------------------------------------------------------------------------

    /// Where the camera starts, and the middle of the map.
    @Export var mapCentre: Vec3 = Vec3(x: 16, y: 0, z: 16)
    /// The starting squad's two positions.
    @Export var firstUnit: Vec3 = Vec3(x: 12, y: 0, z: 16)
    @Export var secondUnit: Vec3 = Vec3(x: 20, y: 0, z: 16)
    /// Where a unit built with the spawn key appears.
    @Export var barracks: Vec3 = Vec3(x: 8, y: 0, z: 24)

    @Export(range: 1...60) var panSpeed: Float = 12
    @Export(range: 0...64) var edgeBand: Float = 8

    @Export(range: 0.5...20) var unitSpeed: Float = 6
    @Export(range: 0.1...2) var unitRadius: Float = 0.5
    @Export(range: 0.5...4) var unitHeight: Float = 1.8
    @Export(range: 0.05...2) var arrivalDistance: Float = 0.3

    /// What a worker costs, and the stock the game starts with.
    @Export var workerCost: Float = 50
    @Export var startingGold: Float = 1250
    @Export var startingWood: Float = 830
    @Export var foodCap: Float = 10
    /// The map's side in metres, for placing units on the minimap.
    @Export var mapSize: Float = 32

    /// The level's barracks node, resolved by the engine at `onReady`. Nil if the level has none.
    @Node("../Barracks") var barracksNode: Entity?

    // --- State ---------------------------------------------------------------------------------

    private var worker = Prefab(raw: 0)
    private var arrivedCue = AudioCue(raw: 0)
    private var squad: [Entity] = []
    private var selected: Entity = .null
    /// A ground point clicked in `onUpdate`, waiting for the next fixed step.
    private var pendingOrder: Vec3?
    private var camera = RtsCamera(focus: Vec3(), speed: 12, edgeBand: 8)
    /// The HUD, or nil when the engine has no interface.
    private var hud: RtsHud?
    /// The Build button was clicked; the next fixed step builds.
    private var buildRequested = false
    /// Each unit's health: the starting squad is one healthy worker and one hurt one, and a new
    /// worker arrives at less than full strength.
    private var health: [Entity: (current: UInt32, max: UInt32)] = [:]

    /// ABI 1.8: the lockstep company, ordered in fixed point (Company.swift).
    private var company = Company()

    /// The state each unit was last asked to play, so a request is made only when it changes.
    private var playing: [Entity: String] = [:]
    /// Units whose cheer has finished, waiting for the next fixed step to stand them down.
    private var cheered: Set<Entity> = []

    private var tally = Tally()
    private var report = ComponentType(id: 0)
    private var reportFields = ReportFields()

    // --- Lifecycle -----------------------------------------------------------------------------

    override func onCreate() throws {
        guard let world else { throw CyberdyneError.invalidHandle }
        report = try Components.register(RtsReport.self, in: world)
        if !world.has(report, on: entity) {
            try world.add(report, to: entity)
        }
        reportFields = ReportFields()

        // Resolved once, here, where the engine may still load. A fixed step would get
        // `.unavailable` for an asset that is not resident rather than stall on it.
        worker = try Spawn.prefab(Content.worker)
        arrivedCue = try Audio.cue(Content.arrived)
        camera = RtsCamera(focus: mapCentre, speed: panSpeed, edgeBand: edgeBand)

        let starting = try worker.instantiate(at: [
            Pose(position: firstUnit), Pose(position: secondUnit),
        ])
        for (unit, hp) in zip(starting, [UInt32(100), 64]) {
            try enlist(unit, health: hp)
        }
        hud = try mountHud()
        _ = try company.enlist()
        Log.info("Commander: \(squad.count) units ready, \(company.units.count) in the company")
    }

    /// The HUD, with the Build button wired to the next fixed step. Nil, and said once, when the
    /// engine has no interface to build it in.
    private func mountHud() throws -> RtsHud? {
        do {
            return try RtsHud(for: self) { [unowned self] in self.buildRequested = true }
        } catch CyberdyneError.status(.unavailable, _) {
            Log.info("Commander: the engine has no interface; playing without a HUD")
            return nil
        }
    }

    override func onEnterTree() throws {
        tally.entered += 1
        try publish()
    }

    /// Every `@Node` is resolved before this runs, so the barracks reference is already filled in.
    override func onReady() throws {
        tally.readied += 1
        tally.barracksFound = barracksNode == nil ? 0 : 1
        try publish()
    }

    override func onUpdate(_ delta: Double) throws {
        guard let active = try Camera.active() else { return }
        let pointer = try Input.pointer()
        let keys = try Input.action(Content.pan).axis2
        let viewport = try active.view().viewport
        let direction = camera.direction(keys: keys, pointer: pointer, viewport: viewport)
        try camera.pan(active, direction: direction, delta: Float(delta))

        let onHud = try hud != nil && UI.hitTest(pointer.position) != nil
        if pointer.pressed.contains(.left) && !onHud {
            try select(under: pointer, through: active)
        }
        if pointer.pressed.contains(.right) && !selected.isNull && !onHud {
            try order(under: pointer, through: active)
        }
        // The animation events of the ticks since the last frame, each delivered once.
        for event in try Animation.events() {
            if event.name == Content.footstep {
                tally.footsteps += 1
            } else if event.name == Content.cheerDone {
                tally.cheerEvents += 1
                cheered.insert(event.entity)
            }
        }
        try hud?.show(model())
    }

    /// Every click on the HUD, counted for the report. A button's own action — Build's — has run
    /// by the time this is called.
    override func onUIEvent(_ event: UIEvent) throws {
        if event.kind == .click {
            tally.hudClicks += 1
        }
    }

    override func onFixedUpdate(_ delta: Double) throws {
        // A click that deselected after the order was recorded leaves nobody to send.
        if let target = pendingOrder, !selected.isNull {
            try NavAgent(selected).move(to: target)
            tally.orders += 1
        }
        pendingOrder = nil
        try company.step()
        for unit in squad {
            try listen(to: unit)
        }
        let built = buildRequested
        buildRequested = false
        if try Input.action(Content.spawn).justPressed || built {
            try enlist(worker.instantiate(at: Pose(position: barracks)), health: 30)
            tally.spawns += 1
            tally.hudBuilds += built ? 1 : 0
        }
        try publish()
    }

    // --- Frame: the pointer --------------------------------------------------------------------

    /// Left click: the unit under the pointer, or nothing when the click missed every unit.
    private func select(under pointer: Pointer, through camera: Camera) throws {
        let ray = try camera.ray(under: pointer)
        let hit = try Physics.raycast(ray, filter: Layers.units)
        selected = hit.map(\.entity).flatMap { squad.contains($0) ? $0 : nil } ?? .null
    }

    /// Right click: remember the ground point under the pointer as the selected unit's order.
    private func order(under pointer: Pointer, through camera: Camera) throws {
        let ray = try camera.ray(under: pointer)
        if let ground = try Physics.raycast(ray, filter: Layers.ground) {
            pendingOrder = ground.point
        }
    }

    // --- Fixed step: the squad -----------------------------------------------------------------

    /// Make `unit` a navigation agent with this game's size and speed, enrol it in `trainUnits`
    /// by giving it a `Veterancy`, and count it in the squad.
    private func enlist(_ unit: Entity, health hp: UInt32) throws {
        try NavAgent(unit).configure(
            .init(
                radius: unitRadius, height: unitHeight, maxSpeed: unitSpeed,
                arrivalDistance: arrivalDistance))
        if let world {
            try world.add(Components.register(Veterancy.self, in: world), to: unit)
        }
        // The engine animates it from now on; the game only says which state to play.
        try Animator.attach(to: unit, rig: Content.workerRig)
        playing[unit] = Content.idle
        squad.append(unit)
        health[unit] = (hp, 100)
    }

    /// Play the arrival cue where a unit stopped, on the one tick navigation reports it, and the
    /// animation its agent's state calls for.
    private func listen(to unit: Entity) throws {
        let state = try NavAgent(unit).state
        if state.justArrived {
            tally.arrivals += 1
            if try Audio.play(arrivedCue, at: state.position) != nil {
                tally.cues += 1
            }
            try play(Content.cheer, on: unit, crossfade: 0.1)
            return
        }
        if playing[unit] == Content.cheer {
            // The cheer holds until its own event says it is over.
            guard cheered.remove(unit) != nil else { return }
            try play(Content.idle, on: unit, crossfade: 0.25)
            return
        }
        let moving = state.status == .following || state.status == .computing
        try play(moving ? Content.walk : Content.idle, on: unit, crossfade: 0.2)
    }

    /// Ask for `state` on `unit` when it is not what the unit is already playing.
    private func play(_ state: String, on unit: Entity, crossfade: Float) throws {
        guard playing[unit] != state else { return }
        try Animator(unit).play(state, crossfade: crossfade)
        playing[unit] = state
    }

    // --- Frame: the HUD -------------------------------------------------------------------------

    /// What the HUD shows this frame: the stock less what was spent, the selected unit, and where
    /// every unit and the camera are on the map.
    private func model() throws -> HudModel {
        var model = HudModel()
        let spent = Float(tally.spawns) * workerCost
        model.resources = .init(
            gold: UInt32(max(0, startingGold - spent)), wood: UInt32(startingWood),
            food: UInt32(squad.count), foodCap: UInt32(foodCap))
        if !selected.isNull, let hp = health[selected] {
            model.units = [.init(name: "Worker", health: hp.current, maxHealth: hp.max)]
        }
        for unit in squad.prefix(RtsHud.minimapDots) {
            let at = try NavAgent(unit).state.position
            model.dots.append(.init(x: at.x / mapSize, y: at.z / mapSize, team: .player))
        }
        // The view the rig frames, about 16 by 12 metres round the focus.
        let focus = camera.focus
        model.camera = UIRect(
            x: (focus.x - 8) / mapSize, y: (focus.z - 6) / mapSize, width: 16 / mapSize,
            height: 12 / mapSize)
        return model
    }

    private func publish() throws {
        guard let world else { return }
        try world.setValue(.entity(selected), entity, report, field: reportFields.selected)
        try world.setFloat(tally.orders, entity, report, field: reportFields.orders)
        try world.setFloat(tally.arrivals, entity, report, field: reportFields.arrivals)
        try world.setFloat(tally.cues, entity, report, field: reportFields.cues)
        try world.setFloat(tally.spawns, entity, report, field: reportFields.spawns)
        try world.setFloat(tally.entered, entity, report, field: reportFields.entered)
        try world.setFloat(tally.readied, entity, report, field: reportFields.readied)
        try world.setFloat(tally.barracksFound, entity, report, field: reportFields.barracksFound)
        try world.setFloat(tally.hudBuilds, entity, report, field: reportFields.hudBuilds)
        try world.setFloat(tally.hudClicks, entity, report, field: reportFields.hudClicks)
        try world.setFloat(hud == nil ? 0 : 1, entity, report, field: reportFields.hud)
        try world.setFloat(tally.footsteps, entity, report, field: reportFields.footsteps)
        try world.setFloat(tally.cheerEvents, entity, report, field: reportFields.cheerEvents)
        try world.setFloat(company.orders, entity, report, field: reportFields.companyOrders)
        try world.setFloat(company.arrivals, entity, report, field: reportFields.companyArrivals)
        try world.setFixed(company.lead, entity, report, field: reportFields.companyLead)
    }
}

/// The counters `RtsReport` carries.
private struct Tally {
    var orders: Float = 0
    var arrivals: Float = 0
    var cues: Float = 0
    var spawns: Float = 0
    var entered: Float = 0
    var readied: Float = 0
    var barracksFound: Float = 0
    var hudBuilds: Float = 0
    var hudClicks: Float = 0
    var footsteps: Float = 0
    var cheerEvents: Float = 0
}

/// `RtsReport`'s field indices, resolved by name once.
private struct ReportFields {
    let selected = RtsReport.field("selected")
    let orders = RtsReport.field("orders")
    let arrivals = RtsReport.field("arrivals")
    let cues = RtsReport.field("cues")
    let spawns = RtsReport.field("spawns")
    let entered = RtsReport.field("entered")
    let readied = RtsReport.field("readied")
    let barracksFound = RtsReport.field("barracksFound")
    let hudBuilds = RtsReport.field("hudBuilds")
    let hudClicks = RtsReport.field("hudClicks")
    let hud = RtsReport.field("hud")
    let footsteps = RtsReport.field("footsteps")
    let cheerEvents = RtsReport.field("cheerEvents")
    let companyOrders = RtsReport.field("companyOrders")
    let companyArrivals = RtsReport.field("companyArrivals")
    let companyLead = RtsReport.field("companyLead")
}
