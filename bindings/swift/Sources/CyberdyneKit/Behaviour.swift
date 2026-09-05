// Behaviour.swift — the ergonomic, object-oriented programming model. Tasks 3.2, 3.5.
//
// `swift-scripting`: "`Behaviour` SHALL be the ergonomic, object-oriented entry point: a Swift class
// attached to a node, receiving lifecycle callbacks", with the lifecycle `scene-graph-and-nodes`
// defines, and "Only overridden callbacks SHALL be dispatched."
//
// --- HOW "ONLY OVERRIDDEN CALLBACKS ARE DISPATCHED" IS DECIDED -------------------------------------
//
// Not by asking the runtime. There is no portable way in Swift to ask whether a subclass overrode a
// method: `class_getMethodImplementation` is Objective-C, which does not exist on Linux, and
// comparing unapplied method references is not expressible. Every remaining runtime answer goes
// through name-based type lookup, which the hot-reload spike measured returning a RETIRED
// generation's metadata when two images are resident — so a runtime answer here would be wrong in
// exactly the configuration this milestone is built around.
//
// It is decided at COMPILE time instead: the `@Behaviour` macro reads the class body, sees which
// lifecycle functions it declares, and emits `behaviourCallbacks`. The engine then never calls a
// callback the class did not write, and `swift-scripting`'s "SHALL not be registered in the
// per-frame dispatch list" is a fact about the registration rather than a check inside it.
//
// --- WHAT THE ABI CAN DELIVER TODAY ----------------------------------------------------------------
//
// `CyBehaviourVTable` carries `create`, `destroy`, `fixed_update`, `serialize` and `deserialize`.
// So `onCreate`, `onFixedUpdate`, `onDestroy` and `onAfterReload` are driven by the engine now. The
// tree callbacks — `onEnterTree`, `onReady`, `onEnable`, `onDisable`, `onUpdate`, `onExitTree` —
// need scene and frame entries that ABI 1.0's table does not have; they are declared here, they are
// recorded in `behaviourCallbacks`, and `dispatch(_:)` is the seam that drives them. When the
// scene's entries are APPENDED to `CyInterface` (which is the only legal way to grow it), the
// bridge gains the thunks and nothing in a game changes.

import CyberdyneCore

/// Which lifecycle callbacks a behaviour class implements.
public struct CallbackSet: OptionSet, Sendable, Hashable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }

    public static let create = CallbackSet(rawValue: 1 << 0)
    public static let enterTree = CallbackSet(rawValue: 1 << 1)
    public static let ready = CallbackSet(rawValue: 1 << 2)
    public static let enable = CallbackSet(rawValue: 1 << 3)
    public static let disable = CallbackSet(rawValue: 1 << 4)
    public static let fixedUpdate = CallbackSet(rawValue: 1 << 5)
    public static let update = CallbackSet(rawValue: 1 << 6)
    public static let exitTree = CallbackSet(rawValue: 1 << 7)
    public static let destroy = CallbackSet(rawValue: 1 << 8)
    public static let afterReload = CallbackSet(rawValue: 1 << 9)
    public static let migrate = CallbackSet(rawValue: 1 << 10)

    /// The names, for a diagnostic that has to say which callback a behaviour was disabled in.
    public var names: [String] {
        let table: [(CallbackSet, String)] = [
            (.create, "onCreate"), (.enterTree, "onEnterTree"), (.ready, "onReady"),
            (.enable, "onEnable"), (.disable, "onDisable"), (.fixedUpdate, "onFixedUpdate"),
            (.update, "onUpdate"), (.exitTree, "onExitTree"), (.destroy, "onDestroy"),
            (.afterReload, "onAfterReload"), (.migrate, "onMigrate"),
        ]
        return table.filter { contains($0.0) }.map(\.1)
    }
}

/// The base class of every Swift behaviour.
///
/// It holds a handle and never an owning reference. `swift-scripting`: "A `Node` or entity wrapper
/// is a **handle**, not an owning reference; holding one does not keep the entity alive", and
/// "the engine SHALL hold behaviours weakly from the entity side" — which on this side of the
/// boundary means the engine holds the instance through `CyInstance`, an opaque pointer it never
/// dereferences, and the ONLY strong reference is the one the bridge takes at `create` and gives
/// back at `destroy`.
open class Behaviour {
    /// The entity this behaviour is attached to.
    public let entity: Entity

    /// The world, re-read rather than stored: an entity outlives no world, but a world binding can
    /// appear after a module is brought up at `CY_INIT_LEVEL_CORE`.
    public var world: World? { Runtime.world }

    /// False once a callback threw and the bridge disabled this instance. `swift-scripting`: the
    /// engine "SHALL disable that behaviour rather than terminating the process".
    public internal(set) var isEnabled: Bool = true

    public required init(entity: Entity) {
        self.entity = entity
    }

    // The lifecycle `scene-graph-and-nodes` defines. Every one is a no-op here, and a class that
    // does not write one is not registered for it — see `behaviourCallbacks` above.
    //
    // THEY THROW, AND THAT IS THE WHOLE "DISABLE RATHER THAN TERMINATE" MECHANISM. A callback that
    // reports failure by throwing gives the bridge a value to catch, a message to log and an
    // instance to disable; a callback that could only trap would take the engine's process with it.
    // Overriding a throwing method with a non-throwing one is legal, so a behaviour that never
    // fails writes `override func onFixedUpdate(_ dt: Double)` and pays nothing.
    open func onCreate() throws {}
    open func onEnterTree() throws {}
    open func onReady() throws {}
    open func onEnable() throws {}
    open func onDisable() throws {}
    open func onFixedUpdate(_ delta: Double) throws {}
    open func onUpdate(_ delta: Double) throws {}
    open func onExitTree() throws {}
    open func onDestroy() throws {}

    /// Called after a hot reload has restored what it could. `swift-scripting`: "private state that
    /// cannot be serialized ... SHALL be reinitialised on reload and the behaviour SHALL be given a
    /// `onAfterReload` callback to rebuild it."
    ///
    /// `restored` names the exported properties the blob actually carried, so a behaviour can tell
    /// a field that was defaulted from one that was saved with its default value.
    open func onAfterReload(restored: Set<String>) throws {}

    /// Called for a saved entry that no `@Export`ed property claims, before `onAfterReload`.
    ///
    /// THIS IS WHERE A RENAME LIVES, AND IT HAS TO LIVE SOMEWHERE. Restoring by name makes adding
    /// and removing a field free, and it makes RENAMING one a silent loss: the old key matches
    /// nothing, the new property keeps its default, and no reader of either schema can tell. The
    /// spike's C module carried the same seam — schema 1's `ammo` becomes schema 2's `mana`,
    /// halved — expressed in the code that knows both shapes, and nowhere else.
    ///
    /// Return true when the entry was consumed, so that `onAfterReload` sees it in `restored`.
    open func onMigrate(_ key: String, _ value: Value) throws -> Bool { false }
}

/// What the `@Behaviour` macro adds to a class, and what the bridge needs to register one.
///
/// Written as a protocol rather than as class members so that the macro's output has a name to
/// conform to and so that a hand-written registration — which is what the reload fixture and the
/// engine's own tests use, precisely so that the macro is not in the trusted path of the loader
/// tests — is the same shape as a generated one.
public protocol BehaviourClass: Behaviour {
    /// The initialiser the bridge builds an instance with.
    ///
    /// Declared here as well as on `Behaviour` so that it can be called on an EXISTENTIAL metatype:
    /// a game hands over `[any BehaviourClass.Type]` and the registrar has to construct from it,
    /// which needs the requirement on the protocol rather than only on the base class.
    init(entity: Entity)

    /// The name the engine registers this type under. Stable across reloads: it is how an instance
    /// finds its type again in the next generation.
    static var behaviourName: String { get }
    /// Bumped whenever the serialized shape changes. The reader compares it with the blob's; see
    /// Serialization.swift, item 3.
    static var behaviourSchema: UInt32 { get }
    /// Which callbacks this class implements.
    static var behaviourCallbacks: CallbackSet { get }
    /// The `@Export`ed property names, in declaration order.
    static var exportedNames: [String] { get }
    /// The storage behind one exported property, or nil when this class has no such property.
    func exportedStorage(named name: String) -> (any ExportedStorage)?
}

extension BehaviourClass {
    /// Every exported property as a (name, value) pair, in declaration order.
    public func exportedValues() -> [(String, Value)] {
        Self.exportedNames.compactMap { name in
            exportedStorage(named: name).map { (name, $0.exportedValue) }
        }
    }
}
