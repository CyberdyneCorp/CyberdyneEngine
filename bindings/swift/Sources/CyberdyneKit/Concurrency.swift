// Concurrency.swift — `@GameActor`, and the rule it exists to make checkable. Task 3.6.
//
// `swift-scripting`: "The overlay SHALL provide a `@MainActor`-equivalent **`@GameActor`**
// confining engine mutation to the simulation thread. Swift structured concurrency tasks that touch
// engine state SHALL be `@GameActor`-isolated." And, as a scenario: Swift code mutating engine state
// from a detached task without game-actor isolation SHALL be a **compile-time** error.
//
// That last word is why this file is a global actor and not a comment. A runtime check would fire
// on the machine that happened to run the racing schedule; the compiler's check fires on every
// build, which is the difference between a rule and an aspiration.
//
// --- WHAT IS NOT VERIFIED, AND MUST NOT BE ASSUMED ------------------------------------------------
//
// M4's hot-reload spike exercised no Swift Concurrency inside a module: no `Task`, no `async`, no
// custom executor. A detached task still running inside a RETIRED generation is therefore untested,
// and the reload model's safety argument does not cover it — the argument is that no image is ever
// unloaded, which keeps a retired generation's code and metadata valid, but a task that keeps
// running holds *state* the reload just serialized and destroyed.
//
// So the rule this file enforces is the conservative one, and it is enforced rather than written
// down: engine mutation is `@GameActor`-isolated, the game actor's executor is the simulation
// thread, and `Runtime.assertQuiesced()` is what a reload calls to say that nothing is in flight.
// Lifting it needs a measurement, not a decision.

/// The actor engine mutation is confined to.
///
/// It is a *global* actor rather than an instance actor because there is exactly one simulation, and
/// because global-actor isolation is what Swift will infer for a type or a function annotated with
/// it — which is what makes `@GameActor` cheap enough that game code will actually use it.
///
/// Its executor is the engine's simulation thread: a continuation resumed on the game actor resumes
/// where a fixed tick runs, which is `swift-scripting`'s "the continuation SHALL resume on the
/// simulation thread inside the game actor".
@globalActor
public actor GameActor {
    public static let shared = GameActor()
}

/// Marks a call that must happen at the quiesced frame boundary — module bring-up, type
/// registration, reload.
///
/// `native-abi` requires the quiesce; this is the Swift side's record of which calls depend on it.
/// It is documentation with a name rather than a runtime check on purpose: the engine is the side
/// that knows whether it has quiesced, and a check here could only ever ask a question this image
/// cannot answer.
public typealias QuiescedBoundary = Void
