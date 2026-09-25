import CyberdyneKit
import Foundation

/// Rotates the attached scene node around its local Z axis during fixed Play ticks.
@Behaviour(name: "SpinCube", schema: 1)
final class SpinCube: Behaviour {
    @Export(range: 0...360) var degreesPerSecond: Float = 45

    private var angle: Float = 0
    private var transform = ComponentType.invalid

    override func onCreate() throws {
        guard let world else { throw SpinError.missingWorld }
        transform = world.find(component: "cy::scene::LocalTransform")
        guard transform.isValid else { throw SpinError.missingTransform }
    }

    override func onFixedUpdate(_ delta: Double) throws {
        guard let world else { throw SpinError.missingWorld }
        angle += degreesPerSecond * Float(delta) * .pi / 180
        let half = angle * 0.5
        // The reflected LocalTransform fields begin with rotation x, y, z, w.
        try world.setFloat(sin(half), entity, transform, field: 2)
        try world.setFloat(cos(half), entity, transform, field: 3)
    }
}

private enum SpinError: Error {
    case missingWorld
    case missingTransform
}

@GameModule
enum EditorDemoGame: GameModule {
    static let behaviours: [any BehaviourClass.Type] = [SpinCube.self]
}
