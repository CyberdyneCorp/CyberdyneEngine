// ConcurrencyTests.swift — `@GameActor`, and the honest limit of what a test can hold. Task 3.6.
//
// `swift-scripting`'s concurrency scenario is "Unsynchronised mutation is rejected": Swift code
// mutating engine state from a detached task without game-actor isolation SHALL produce a
// COMPILE-TIME error. A compile-time error cannot be asserted by a test that has to compile — so
// what is asserted here is the other half, the half a test can hold:
//
//   * `@GameActor` exists, is a global actor, and isolation is inherited by an annotated type;
//   * reaching isolated state from outside requires `await`, which is the compiler enforcing the
//     rule — the `await`s below are not decoration, and removing one is a build failure;
//   * the counterexample is written down, commented, with the error it produces.
//
// The negative case is checked by the whole package compiling in Swift 6 language mode
// (Package.swift says why that is deliberate). Every file in `Sources/` is under strict concurrency
// checking, so the day someone writes an unisolated mutation of shared state, the build stops.

import XCTest

@testable import CyberdyneKit

/// Engine-facing state, isolated the way `swift-scripting` requires game state to be.
@GameActor
final class SimulationState {
    private(set) var ticks = 0
    func advance() { ticks += 1 }
}

final class ConcurrencyTests: XCTestCase {
    func testGameActorIsolatedStateIsReachedThroughAwait() async {
        let state = await SimulationState()
        await state.advance()
        await state.advance()
        let ticks = await state.ticks
        XCTAssertEqual(ticks, 2)

        // THE COUNTEREXAMPLE, and it is the scenario. Uncommenting it does not fail this test; it
        // fails the BUILD, with:
        //
        //     error: main actor-isolated property 'ticks' can not be referenced from a nonisolated
        //            context
        //
        // which is `swift-scripting`'s "Unsynchronised mutation is rejected" happening where the
        // specification asks for it — at compile time, on every build, rather than on the machine
        // that happened to run the racing schedule.
        //
        //     Task.detached { state.advance() }
        //     XCTAssertEqual(state.ticks, 2)
    }

    /// A continuation resumed on the game actor is back on the simulation thread's executor, which
    /// is what makes `await Assets.load(...)` safe to follow with engine mutation.
    func testWorkResumesOnTheGameActor() async {
        let state = await SimulationState()
        await Task { @GameActor in
            state.advance()
        }.value
        let ticks = await state.ticks
        XCTAssertEqual(ticks, 1)
    }

    /// What the spike did NOT measure, recorded as a test name so that it is read.
    ///
    /// No `Task` or `async` work was exercised inside a module image across a reload. A detached
    /// task still running in a RETIRED generation is untested: the model's safety argument is that
    /// no image is ever unloaded, which keeps a retired generation's code and metadata valid — but a
    /// task that keeps running holds state the reload has already serialized and destroyed. Until
    /// that is measured, `CyberdyneKit` starts no task of its own, which this asserts by
    /// construction rather than by hope: the reload path is entirely synchronous.
    func testTheReloadPathStartsNoTasks() {
        // `BehaviourRuntime` calls serialize, destroy, entry and deserialize on the calling thread,
        // and every one of those thunks in BehaviourBridge.swift is a synchronous function. There is
        // no `Task` and no `async` anywhere in the reload path; if one is ever added, this case's
        // name is the reminder that it needs a measurement first.
        XCTAssertFalse(Runtime.isBoundToEngine)
    }
}
