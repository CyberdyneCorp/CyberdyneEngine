// SPDX-License-Identifier: MIT
import CyberdyneKit
import Foundation

/// The Swift twin of the gameplay graph `game/scripts/unit_command.cyscript`, issue #29: given an
/// order to a ground point, move there at `speed`, then play the `unit.arrived` cue where it
/// stopped. The order is the exported target; the graph receives it as its `unit.command` event.
///
/// The step is `cy::game_backend::step_towards` written in Swift, expression for expression, so
/// the two units are comparable float for float rather than within a tolerance: the
/// `smoke.editor_graph_equivalence` suite runs both and requires the same positions on every tick
/// and the same cue on the same tick.
@Behaviour(name: "CommandedUnit", schema: 1)
final class CommandedUnit: Behaviour {
    @Export var targetX: Float = 0
    @Export var targetZ: Float = 0
    @Export var speed: Float = 3

    private var transform = ComponentType.invalid
    private var moving = true

    override func onCreate() throws {
        guard let world else { throw CommandedUnitError.missingWorld }
        transform = world.find(component: "cy::scene::LocalTransform")
        guard transform.isValid else { throw CommandedUnitError.missingTransform }
    }

    override func onFixedUpdate(_ delta: Double) throws {
        guard moving, let world else { return }
        // The reflected LocalTransform fields are rotation x, y, z, w, then translation x, y, z.
        var x = try world.float(entity, transform, field: 4)
        let y = try world.float(entity, transform, field: 5)
        var z = try world.float(entity, transform, field: 6)
        let dx = targetX - x
        let dz = targetZ - z
        let distance = ((dx * dx) + (dz * dz)).squareRoot()
        let step = speed * Float(delta)
        if distance <= step {
            x = targetX
            z = targetZ
            moving = false
        } else {
            x += dx / distance * step
            z += dz / distance * step
        }
        try world.setFloat(x, entity, transform, field: 4)
        try world.setFloat(z, entity, transform, field: 6)
        if !moving {
            try Audio.play("unit.arrived", at: Vec3(x: x, y: y, z: z))
        }
    }
}

private enum CommandedUnitError: Error {
    case missingWorld
    case missingTransform
}
