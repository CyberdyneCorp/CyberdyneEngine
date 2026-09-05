// Generation 1 of the reload fixture. Schema 2: `ammo` became `mana` (halved), `shield` is new and
// defaults, and the stored layout is therefore different from generation 0's.
import CyberdyneKit

@Behaviour(name: "SwiftCounter", schema: 2)
final class SwiftCounter: Behaviour {
    @Export var health: Int64 = 95
    @Export var mana: Int64 = 0
    /// A field schema 1 did not have. A restored instance keeps this default, which is the "new
    /// field defaults" half of a migration.
    @Export var shield: Int64 = 10
    @Export var label: String = "player"

    private(set) var ticks: Int64 = 0
    private(set) var reloadedFrom: Set<String> = []

    override func onFixedUpdate(_ delta: Double) {
        ticks += 1
        health -= 1
    }

    /// The rename, expressed in the code that knows both shapes and nowhere else.
    override func onMigrate(_ key: String, _ value: Value) throws -> Bool {
        guard key == "ammo", case let .i64(ammo) = value else { return false }
        mana = ammo / 2
        return true
    }

    override func onAfterReload(restored: Set<String>) throws {
        reloadedFrom = restored
        Log.info("SwiftCounter restored \(restored.sorted().joined(separator: ", "))")
    }
}

@GameModule
enum ReloadFixture: GameModule {
    static let behaviours: [any BehaviourClass.Type] = [SwiftCounter.self]
}
