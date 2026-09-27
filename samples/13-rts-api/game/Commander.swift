// SPDX-License-Identifier: MIT
// Commander.swift — the player's side of an RTS: a squad, a selection, move orders, a build key and
// a camera. All of it goes through ABI 1.3 by way of CyberdyneKit.
//
// WHICH CALLBACK DOES WHAT, AND WHY. The engine calls this behaviour in three phases, and each game
// service says which phases may call it (openspec/specs/native-abi/spec.md):
//
//   onCreate       (no phase)    resolve the prefab and the cue, spawn the starting squad
//   onUpdate       (frame)       read the pointer, pan the camera, pick a unit or a ground point
//   onFixedUpdate  (fixed step)  hand the recorded order to navigation, hear arrivals, build
//
// The pointer and the camera are device and presentation state, so a fixed step may not read them.
// Spawning changes the simulation, so a frame may not do it. A click is therefore RECORDED in
// `onUpdate` and ACTED ON in the next `onFixedUpdate`, which is the pattern design.md gives for an
// RTS. Everything the fixed step reads (action state, navigation state) replays the same way.

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

    // --- State ---------------------------------------------------------------------------------

    private var worker = Prefab(raw: 0)
    private var arrivedCue = AudioCue(raw: 0)
    private var squad: [Entity] = []
    private var selected: Entity = .null
    /// A ground point clicked in `onUpdate`, waiting for the next fixed step.
    private var pendingOrder: Vec3?
    private var camera = RtsCamera(focus: Vec3(), speed: 12, edgeBand: 8)

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
        for unit in starting {
            try enlist(unit)
        }
        Log.info("Commander: \(squad.count) units ready")
    }

    override func onUpdate(_ delta: Double) throws {
        guard let active = try Camera.active() else { return }
        let pointer = try Input.pointer()
        let keys = try Input.action(Content.pan).axis2
        let viewport = try active.view().viewport
        let direction = camera.direction(keys: keys, pointer: pointer, viewport: viewport)
        try camera.pan(active, direction: direction, delta: Float(delta))

        if pointer.pressed.contains(.left) {
            try select(under: pointer, through: active)
        }
        if pointer.pressed.contains(.right) && !selected.isNull {
            try order(under: pointer, through: active)
        }
    }

    override func onFixedUpdate(_ delta: Double) throws {
        // A click that deselected after the order was recorded leaves nobody to send.
        if let target = pendingOrder, !selected.isNull {
            try NavAgent(selected).move(to: target)
            tally.orders += 1
        }
        pendingOrder = nil
        for unit in squad {
            try listen(to: unit)
        }
        if try Input.action(Content.spawn).justPressed {
            try enlist(worker.instantiate(at: Pose(position: barracks)))
            tally.spawns += 1
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

    /// Make `unit` a navigation agent with this game's size and speed, and count it in the squad.
    private func enlist(_ unit: Entity) throws {
        try NavAgent(unit).configure(
            .init(
                radius: unitRadius, height: unitHeight, maxSpeed: unitSpeed,
                arrivalDistance: arrivalDistance))
        squad.append(unit)
    }

    /// Play the arrival cue where a unit stopped, on the one tick navigation reports it.
    private func listen(to unit: Entity) throws {
        let state = try NavAgent(unit).state
        guard state.justArrived else { return }
        tally.arrivals += 1
        if try Audio.play(arrivedCue, at: state.position) != nil {
            tally.cues += 1
        }
    }

    private func publish() throws {
        guard let world else { return }
        try world.setValue(.entity(selected), entity, report, field: reportFields.selected)
        try world.setFloat(tally.orders, entity, report, field: reportFields.orders)
        try world.setFloat(tally.arrivals, entity, report, field: reportFields.arrivals)
        try world.setFloat(tally.cues, entity, report, field: reportFields.cues)
        try world.setFloat(tally.spawns, entity, report, field: reportFields.spawns)
    }
}

/// The counters `RtsReport` carries.
private struct Tally {
    var orders: Float = 0
    var arrivals: Float = 0
    var cues: Float = 0
    var spawns: Float = 0
}

/// `RtsReport`'s field indices, resolved by name once.
private struct ReportFields {
    let selected = RtsReport.field("selected")
    let orders = RtsReport.field("orders")
    let arrivals = RtsReport.field("arrivals")
    let cues = RtsReport.field("cues")
    let spawns = RtsReport.field("spawns")
}
