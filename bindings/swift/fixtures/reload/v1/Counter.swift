// Generation 0 of the reload fixture. Schema 1: health, ammo, label.
import CyberdyneKit

@Behaviour(name: "SwiftCounter", schema: 1)
final class SwiftCounter: Behaviour {
    @Export var health: Int64 = 95
    @Export var ammo: Int64 = 34
    /// A Swift `String`, so the fixture carries an ARC-managed field across a reload. Reading one
    /// of these as an integer is what produced 3.5e18 in the spike; carrying it by name is what
    /// makes that impossible.
    @Export var label: String = "player"

    /// Not exported: the private state a reload reinitialises, and the reason `onAfterReload`
    /// exists.
    private(set) var ticks: Int64 = 0

    override func onFixedUpdate(_ delta: Double) {
        ticks += 1
        health -= 1
    }
}

@GameModule
enum ReloadFixture: GameModule {
    static let behaviours: [any BehaviourClass.Type] = [SwiftCounter.self]
}
