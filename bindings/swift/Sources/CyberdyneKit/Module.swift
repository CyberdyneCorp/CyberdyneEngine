// Module.swift — `cy_module_entry`, from the Swift side. Tasks 3.2, 3.7.
//
// A game module is a shared library that exports one symbol the loader looks for. Everything about
// its bring-up is stated in `cy/abi/cy_abi.h`; this file is the Swift half of it, and it exists so
// that a game writes a list of behaviour types rather than a C entry point.
//
// --- WHY THE `@_cdecl` LIVES IN THE GAME AND NOT HERE ----------------------------------------------
//
// `CyberdyneKit` is linked into a game module as a static archive, and a linker pulls an object out
// of an archive only when something references a symbol in it. An `@_cdecl("cy_module_entry")`
// sitting in this package would therefore be dropped from the game's `.so` — silently, with the
// loader reporting "did not export its declared entry symbol" and nothing pointing at the cause.
//
// So the two exported functions are emitted into the GAME's own module, by the `@GameModule` macro,
// and they are three lines each. `ModuleBootstrap` below is everything they call.
//
// --- WHY THE REGISTRAR IS A GLOBAL ------------------------------------------------------------------
//
// `CyModuleInit.initialize` is a C function pointer, so it cannot capture. The registrar has to
// reach the game's list of types from a non-capturing thunk, and a global in the module image is
// the only place that can be. It is written once, during `cy_module_entry`, at the quiesced boundary
// — see Runtime.swift for the full argument about `nonisolated(unsafe)` here.

import CyberdyneABI
import CyberdyneCore

/// What a game declares. The `@GameModule` macro emits the two C entry points that drive it.
public protocol GameModule {
    /// The behaviour types this module registers, in the order they should be registered.
    static var behaviours: [any BehaviourClass.Type] { get }

    /// The components this module registers, before its systems — a system's access names its
    /// components, and the engine resolves each in the world when the system is registered.
    static var components: [any Component.Type] { get }

    /// The systems this module asks the engine to schedule: the `@System` macro's
    /// `__CySystem_<name>` for each, e.g. `[__CySystem_ageUnits.self]`. ABI 1.5.
    static var systems: [any SystemRegistration.Type] { get }

    /// Called at each initialisation level. The default registers `components`, `behaviours` and
    /// `systems`, in that order, at `.scene` — where `native-abi` puts type registration and where a
    /// world exists to register against; override it to do more.
    static func initialize(at level: InitLevel)

    /// Called at each level on the way down.
    static func shutdown(at level: InitLevel)
}

extension GameModule {
    /// No components of its own: the default for a module whose systems use none.
    public static var components: [any Component.Type] { [] }
    /// No scheduled systems: the default for a module that only has behaviours.
    public static var systems: [any SystemRegistration.Type] { [] }

    public static func initialize(at level: InitLevel) {
        guard level == .scene else { return }
        if let world = Runtime.world {
            for type in components {
                do {
                    _ = try Components.register(type, in: world)
                } catch {
                    Log.error("could not register component \(type.componentName): \(error)")
                }
            }
        }
        for type in behaviours {
            do {
                try Behaviours.register(type)
            } catch {
                // Reported and survived rather than fatal: a module that fails to register one of
                // its types still has the others, and the engine's log is where a developer will
                // look. Returning false from the entry point is for a version mismatch, which is a
                // different failure and is handled before any of this runs.
                Log.error("could not register \(type.behaviourName): \(error)")
            }
        }
        for system in systems {
            do {
                try system.register()
            } catch {
                // A refused system is logged, not fatal, for the reason a refused behaviour is. A
                // reload that changed a system's stage or access is refused by the engine as a
                // whole, so the previous generation keeps running it.
                Log.error("could not schedule system \(system.descriptor.name): \(error)")
            }
        }
    }

    public static func shutdown(at level: InitLevel) {}
}

/// The engine-facing half of a module's bring-up.
public enum ModuleBootstrap {
    /// The game's hooks, installed by `entry` and read by the C thunks below.
    nonisolated(unsafe) private static var initializer: ((InitLevel) -> Void)?
    nonisolated(unsafe) private static var finalizer: ((InitLevel) -> Void)?

    /// `cy_module_entry`, minus the `@_cdecl`.
    ///
    /// Returns false — which the loader reports and survives — when the engine's table is one this
    /// module may not call. `native-abi`: "Entry point returns false ... reported without aborting".
    public static func entry<Module: GameModule>(
        _ interface: UnsafePointer<CyInterface>?,
        _ engine: CyEngine?,
        _ out: UnsafeMutablePointer<CyModuleInit>?,
        module: Module.Type
    ) -> Bool {
        guard let interface, let engine, let out else { return false }
        guard Runtime.bind(interface: interface, engine: engine) else { return false }

        initializer = Module.initialize(at:)
        finalizer = Module.shutdown(at:)

        out.pointee = CyModuleInit()
        out.pointee.struct_size = UInt32(MemoryLayout<CyModuleInit>.size)
        out.pointee.abi_major = ABI.major
        out.pointee.abi_minor = ABI.minor
        out.pointee.initialize = moduleInitialize
        out.pointee.shutdown = moduleShutdown
        out.pointee.user_data = nil
        return true
    }

    /// `cy_module_shutdown`, minus the `@_cdecl`. Called on the old image immediately before a
    /// reload, after every instance it created has been serialized and destroyed. The image stays
    /// mapped — see cy/abi/module.h — so this drops the engine handles and nothing else.
    public static func shutdown() {
        initializer = nil
        finalizer = nil
        Behaviours.forgetRegistrations()
        Systems.forgetRegistrations()
        Runtime.unbind()
    }

    fileprivate static func runInitialize(_ level: InitLevel) { initializer?(level) }
    fileprivate static func runShutdown(_ level: InitLevel) { finalizer?(level) }
}

private let moduleInitialize:
    @convention(c) (CyEngine?, CyInitLevel, UnsafeMutableRawPointer?)
        -> Void = { _, level, _ in
            guard let level = InitLevel(rawValue: level.rawValue) else { return }
            ModuleBootstrap.runInitialize(level)
        }

private let moduleShutdown:
    @convention(c) (CyEngine?, CyInitLevel, UnsafeMutableRawPointer?)
        -> Void = { _, level, _ in
            guard let level = InitLevel(rawValue: level.rawValue) else { return }
            ModuleBootstrap.runShutdown(level)
        }
