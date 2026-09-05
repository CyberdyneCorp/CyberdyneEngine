// Runtime.swift — the process-wide handles a loaded module reaches the engine through. Task 3.2.
//
// A game module is a shared library the engine opens. It receives the interface table and the
// engine handle exactly once, in `cy_module_entry`, and everything it does afterwards — a
// behaviour's `onFixedUpdate`, a component registration, a log line — needs both. They are
// therefore process-wide state in the module image, and that is not a shortcut: the image IS the
// scope, because `native-abi` gives one module image one engine.
//
// --- WHY THESE ARE `nonisolated(unsafe)` AND WHY THAT IS NOT A HOLE ------------------------------
//
// This package is compiled in Swift 6 language mode, so mutable global state is a compile error
// unless it is either isolated to a global actor or declared `nonisolated(unsafe)`. Isolating these
// to `@GameActor` would be wrong rather than merely awkward: `cy_module_entry` is a C entry point
// the engine calls on its own thread with no actor context, and an actor-isolated global cannot be
// written from there without an `await` that a C function cannot express.
//
// So they are `nonisolated(unsafe)`, and the safety argument is the ABI's own, written down here
// rather than assumed:
//
//   * they are written exactly twice in an image's life — once by `cy_module_entry`, once by
//     `cy_module_shutdown` — and `native-abi` requires both to happen at the quiesced frame
//     boundary, with no engine work in flight;
//   * they are only ever read afterwards, never written, so every access after bring-up is a read
//     of a value that no longer changes;
//   * `cy::abi::BehaviourRuntime` is documented as not thread-safe for the same reason and used at
//     the same boundary, so a lock here would be a lock that is never contended and would still not
//     make the *engine's* side safe.
//
// The rule that does the real work is in Concurrency.swift: engine mutation is confined to
// `@GameActor`, which is what `swift-scripting` asks for and what the compiler can actually check.

import CyberdyneABI
import CyberdyneCore

/// The engine, as one loaded module image sees it.
public enum Runtime {
    /// The table `cy_module_entry` was handed, or nil before bring-up and after shutdown.
    public nonisolated(unsafe) private(set) static var interface: Interface?
    /// The engine handle `cy_module_entry` was handed.
    public nonisolated(unsafe) private(set) static var engineHandle: CyEngine?

    /// The generation this image belongs to, counted by the loader and reported for diagnostics.
    /// It is *not* read from the engine: `behaviour_generation` answers per behaviour type, and a
    /// module that has registered nothing yet has no type to ask about.
    public nonisolated(unsafe) internal(set) static var generation: UInt32 = 0

    /// True once `cy_module_entry` has accepted the table.
    public static var isBoundToEngine: Bool { interface != nil && engineHandle != nil }

    /// The typed engine wrapper, or nil before bring-up.
    public static var engine: Engine? {
        guard let interface, let engineHandle else { return nil }
        return Engine(engineHandle, interface)
    }

    /// The world the engine is running, or nil when none is bound.
    ///
    /// Re-read on every access rather than cached: `engine_world` may answer nil before the world
    /// exists and non-nil afterwards, and a module brought up at `CY_INIT_LEVEL_CORE` is brought up
    /// before there is one. A cached nil would then be wrong for the rest of the process.
    public static var world: World? {
        guard let engine, let raw = engine.world() else { return nil }
        return World(raw, engine.interface)
    }

    /// Bind the table and the engine. Called by the module entry point and by tests; returns false
    /// when the engine's table is one this overlay may not call, which is what `cy_module_entry`
    /// reports by returning false.
    @discardableResult
    static func bind(interface table: UnsafePointer<CyInterface>, engine handle: CyEngine) -> Bool {
        let candidate = Interface(table)
        guard candidate.isCompatible else {
            // Reported through the engine's own last-error slot rather than to stderr, so the
            // loader's message and the module's message are the same sentence. `native-abi`:
            // "Entry point returns false" is reported and aborts nothing.
            let message = "this module was built against ABI \(ABI.major).\(ABI.minor) with a "
                + "\(ABI.interfaceTableSize)-byte table; the engine exports "
                + "\(candidate.abiMajor).\(candidate.abiMinor) with \(candidate.tableSize) bytes"
            message.withCString { text in
                candidate.setLastError(result: CY_RESULT_VERSION_MISMATCH, message: text)
            }
            return false
        }
        interface = candidate
        engineHandle = handle
        return true
    }

    /// Forget the engine. `native-abi`'s `cy_module_shutdown`: the image stays mapped, but nothing
    /// in it may reach the engine again.
    static func unbind() {
        interface = nil
        engineHandle = nil
    }
}
