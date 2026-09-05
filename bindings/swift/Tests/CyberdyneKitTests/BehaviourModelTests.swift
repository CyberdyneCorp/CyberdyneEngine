// BehaviourModelTests.swift — the behaviour model without an engine. Tasks 3.2, 3.4, 3.5.
//
// Everything here runs with no engine bound, which is the point: a behaviour's state, its exported
// properties and its callback set are the module's own, and a test that needed a host to check them
// would be testing the host. The engine-facing half — registration, the vtable, a real reload —
// is `integration.swift_reload` in bindings/swift/tests/, where a C++ host loads a real module.

import CyberdyneKit
import XCTest

@Behaviour(schema: 2)
final class TestPlayer: Behaviour {
    @Export var speed: Float = 6.0
    @Export(range: 0 ... 20) var jumpVelocity: Float = 8.0
    @Export var name: String = "unnamed"

    /// Not exported: private state that a reload reinitialises. `swift-scripting`'s
    /// "Non-representable state is dropped".
    private(set) var ticks: Int = 0
    private(set) var reloaded: Set<String>?

    override func onFixedUpdate(_ delta: Double) {
        ticks += 1
    }

    override func onAfterReload(restored: Set<String>) {
        reloaded = restored
    }
}

@Behaviour
final class TestSilent: Behaviour {}

final class BehaviourModelTests: XCTestCase {
    func testTheMacroNamesTheClassAndTheSchema() {
        XCTAssertEqual(TestPlayer.behaviourName, "TestPlayer")
        XCTAssertEqual(TestPlayer.behaviourSchema, 2)
        XCTAssertEqual(TestSilent.behaviourSchema, 1, "the default schema is 1")
    }

    /// `swift-scripting`: "Unimplemented callback costs nothing" — a behaviour that does not
    /// override `onUpdate` is not registered for it. The set is computed at compile time from the
    /// class body; this is the assertion that it is computed correctly.
    func testOnlyImplementedCallbacksAreDeclared() {
        XCTAssertEqual(TestPlayer.behaviourCallbacks, [.fixedUpdate, .afterReload])
        XCTAssertFalse(TestPlayer.behaviourCallbacks.contains(.update))
        XCTAssertEqual(TestSilent.behaviourCallbacks, [])
        XCTAssertEqual(TestPlayer.behaviourCallbacks.names, ["onFixedUpdate", "onAfterReload"])
    }

    func testExportedPropertiesAreNamedInDeclarationOrder() {
        XCTAssertEqual(TestPlayer.exportedNames, ["speed", "jumpVelocity", "name"])
        XCTAssertEqual(TestSilent.exportedNames, [])
    }

    func testExportedStorageReadsAndWritesTheProperty() {
        let player = TestPlayer(entity: Entity(bits: 1))
        XCTAssertEqual(player.exportedStorage(named: "speed")?.exportedValue, .f32(6.0))
        XCTAssertNil(player.exportedStorage(named: "ticks"),
                     "a property with no @Export must not be reachable by name")

        XCTAssertTrue(player.exportedStorage(named: "speed")?.assign(.f32(9.5)) ?? false)
        XCTAssertEqual(player.speed, 9.5)
    }

    /// A field whose type changed between schemas is REFUSED rather than reinterpreted. That is the
    /// whole reason the blob carries a kind per entry — see Serialization.swift, item 2.
    func testAssigningTheWrongKindIsRefused() {
        let player = TestPlayer(entity: Entity(bits: 1))
        XCTAssertFalse(player.exportedStorage(named: "speed")?.assign(.string("fast")) ?? true)
        XCTAssertEqual(player.speed, 6.0, "the property keeps its value when the kind is wrong")
    }

    func testConstraintsReachTheStorage() {
        let player = TestPlayer(entity: Entity(bits: 1))
        XCTAssertEqual(player.exportedStorage(named: "jumpVelocity")?.exportedConstraint,
                       .range(0 ... 20))
        // Spelled in full: a bare `.none` in a comparison against an Optional is `Optional.none`,
        // which would assert that the property has no storage rather than that it has no constraint.
        XCTAssertEqual(player.exportedStorage(named: "speed")?.exportedConstraint,
                       ExportConstraint.none)
    }

    func testExportedValuesAreOrderedAndComplete() {
        let player = TestPlayer(entity: Entity(bits: 1))
        player.speed = 1.5
        player.name = "Ripley"
        let values = player.exportedValues()
        XCTAssertEqual(values.map(\.0), ["speed", "jumpVelocity", "name"])
        XCTAssertEqual(values.map(\.1), [.f32(1.5), .f32(8.0), .string("Ripley")])
    }

    func testABehaviourHoldsAHandleAndNotAnOwningReference() {
        let player = TestPlayer(entity: Entity(bits: 42))
        XCTAssertEqual(player.entity.bits, 42)
        XCTAssertNil(player.world, "with no engine bound there is no world, and asking is not a trap")
        XCTAssertTrue(player.isEnabled)
    }

    func testLifecycleCallbacksRunThroughTheClass() throws {
        let player = TestPlayer(entity: Entity(bits: 1))
        try player.onFixedUpdate(1.0 / 60.0)
        try player.onFixedUpdate(1.0 / 60.0)
        XCTAssertEqual(player.ticks, 2)
        try player.onAfterReload(restored: ["speed"])
        XCTAssertEqual(player.reloaded, ["speed"])
    }
}

/// `dispatch(_:)` — the seam the tree callbacks arrive through. Task 3.2.
final class BehaviourDispatchTests: XCTestCase {
    /// A behaviour that fails on purpose, to hold the "disable rather than terminate" rule.
    @Behaviour
    final class TestFailing: Behaviour {
        struct Boom: Error {}
        private(set) var readyCount = 0

        override func onReady() throws {
            readyCount += 1
            throw Boom()
        }
    }

    func testOnlyImplementedCallbacksAreDispatched() {
        let player = TestPlayer(entity: Entity(bits: 1))
        player.dispatch(.update, delta: 1)      // not implemented: must not run
        player.dispatch(.fixedUpdate, delta: 1) // implemented
        player.dispatch(.fixedUpdate, delta: 1)
        XCTAssertEqual(player.ticks, 2)
    }

    /// `swift-scripting`: a behaviour that fails is disabled, and the engine keeps running.
    func testAThrowingCallbackDisablesTheInstance() {
        let failing = TestFailing(entity: Entity(bits: 1))
        XCTAssertTrue(failing.isEnabled)
        failing.dispatch(.ready)
        XCTAssertEqual(failing.readyCount, 1)
        XCTAssertFalse(failing.isEnabled, "a thrown error must disable the instance")

        // And a disabled instance is out of dispatch: the second call does not reach the callback.
        failing.dispatch(.ready)
        XCTAssertEqual(failing.readyCount, 1)
    }

    /// `swift-scripting`: "@Node(path) ... SHALL be resolved at `onReady` and be `nil` if the path
    /// does not resolve, rather than trapping".
    ///
    /// THE NIL HALF IS THE WHOLE OF WHAT SHIPS, and this case is why the wrapper is not dead code:
    /// ABI 1.0 has no node entry, so nothing in the engine calls `resolve(_:)` yet and every `@Node`
    /// reads nil. That is the correct answer for an unresolvable path, so the shipped behaviour is
    /// the specification's failure case rather than a stub that traps — and the assertion below is
    /// what holds it to that when the resolver lands.
    func testANodeReferenceIsNilUntilItIsResolvedAndNeverTraps() {
        let reference = Node<TestPlayer>("../Camera")
        XCTAssertEqual(reference.path, "../Camera")
        XCTAssertNil(reference.wrappedValue, "unresolved reads nil rather than trapping")

        let target = TestPlayer(entity: Entity(bits: 7))
        reference.resolve(target)
        XCTAssertTrue(reference.wrappedValue === target)

        // A path that stops resolving — the node was destroyed — is nil again, not a dangling
        // reference. The wrapper holds no ownership either way: `swift-scripting`'s "holding one
        // does not keep the entity alive".
        reference.resolve(nil)
        XCTAssertNil(reference.wrappedValue)
    }
}
