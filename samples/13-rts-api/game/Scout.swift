// SPDX-License-Identifier: MIT
// Scout.swift — a hero walked by a character controller, and a crate pushed with an impulse. ABI 1.5
// by way of CyberdyneKit (`add-swift-m12-gaps`).
//
//   onCreate       (no phase)    make the hero: an entity with a character controller
//   onReady        (the pump)    `@Node("../Crate")` is resolved first; note whether it was
//   onFixedUpdate  (fixed step)  walk the hero, jump once, kick the crate once
//
// The host builds the level — the crate is a node with a dynamic body — and attaches this behaviour
// to `/Level/Scout`. Where the hero walks, when it jumps and how hard the crate is kicked are
// decided here. A move is one fixed step by definition, so it is only ever made in `onFixedUpdate`.

import CyberdyneKit

@Behaviour(name: "Scout", schema: 1)
final class Scout: Behaviour {

    // --- Tunables ------------------------------------------------------------------------------

    /// Where the hero stands at the start: the capsule's centre, just above the ground.
    @Export var heroStart: Vec3 = Vec3(x: 4, y: 0.95, z: 28)
    /// Metres per second along +X while walking.
    @Export(range: 0...10) var walkSpeed: Float = 2
    /// Fixed ticks spent walking, the tick of the one jump, and its speed.
    @Export var walkTicks: Float = 120
    @Export var jumpTick: Float = 30
    @Export(range: 0...20) var jumpSpeed: Float = 4
    /// The tick the crate is kicked, and the impulse, in newton seconds along +Z.
    @Export var kickTick: Float = 200
    @Export(range: 0...500) var kickImpulse: Float = 100

    /// The level's crate, resolved by the engine at `onReady`.
    @Node("../Crate") var crate: Entity?

    // --- State ---------------------------------------------------------------------------------

    private var hero = Entity.null
    private var ticks: Float = 0
    private var tally = ScoutTally()
    private var report = ComponentType(id: 0)
    private let fields = ScoutFields()

    // --- Lifecycle -----------------------------------------------------------------------------

    override func onCreate() throws {
        guard let world else { throw CyberdyneError.invalidHandle }
        report = try Components.register(ScoutReport.self, in: world)
        if !world.has(report, on: entity) {
            try world.add(report, to: entity)
        }
        // The hero is collision layer 2: neither a unit (1) a click selects nor the ground (0) an
        // order lands on, so walking it never changes what the commander's clicks hit.
        hero = Entity(bits: world.createEntity())
        try CharacterController.create(
            on: hero,
            CharacterController.Description(radius: 0.4, layer: 2, start: Pose(position: heroStart))
        )
    }

    override func onReady() throws {
        tally.crateFound = crate == nil ? 0 : 1
        try publish()
    }

    override func onFixedUpdate(_ delta: Double) throws {
        ticks += 1
        let character = CharacterController(hero)
        let walking = ticks <= walkTicks
        try character.move(
            velocity: Vec3(x: walking ? walkSpeed : 0, y: 0, z: 0),
            jump: ticks == jumpTick ? jumpSpeed : nil)
        let state = try character.state
        tally.heroX = state.position.x
        tally.heroGround = Float(state.ground.rawValue)
        if state.ground == .inAir {
            tally.airborne = 1
        }
        if ticks == kickTick, let crate {
            let body = RigidBody(crate)
            try body.applyImpulse(Vec3(x: 0, y: 0, z: kickImpulse))
            tally.kicks += 1
            let velocity = try body.velocity.linear
            tally.crateSpeed = (velocity.x * velocity.x + velocity.z * velocity.z).squareRoot()
        }
        try publish()
    }

    private func publish() throws {
        guard let world else { return }
        let values: [(UInt32, Float)] = [
            (fields.crateFound, tally.crateFound), (fields.heroX, tally.heroX),
            (fields.heroGround, tally.heroGround), (fields.airborne, tally.airborne),
            (fields.kicks, tally.kicks), (fields.crateSpeed, tally.crateSpeed),
        ]
        for (field, value) in values {
            try world.setFloat(value, entity, report, field: field)
        }
    }
}

/// `ScoutReport`'s field indices, resolved by name once.
private struct ScoutFields {
    let crateFound = ScoutReport.field("crateFound")
    let heroX = ScoutReport.field("heroX")
    let heroGround = ScoutReport.field("heroGround")
    let airborne = ScoutReport.field("airborne")
    let kicks = ScoutReport.field("kicks")
    let crateSpeed = ScoutReport.field("crateSpeed")
}

/// The values `ScoutReport` carries.
private struct ScoutTally {
    var crateFound: Float = 0
    var heroX: Float = 0
    var heroGround: Float = -1
    var airborne: Float = 0
    var kicks: Float = 0
    var crateSpeed: Float = 0
}
