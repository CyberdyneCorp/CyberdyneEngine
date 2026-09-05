// BehaviourBridge.swift — a Swift class, as a `CyBehaviourVTable`. Tasks 3.2, 3.5, 3.7.
//
// This is the file where Swift stops. Everything above it is a class with lifecycle methods;
// everything below is five C function pointers and an opaque `void*`, which is what the engine
// stores and hands back.
//
// --- THE OWNERSHIP RULE, IN ONE PARAGRAPH ---------------------------------------------------------
//
// `create` returns `Unmanaged.passRetained(instance).toOpaque()`: the engine's `CyInstance` IS the
// +1 reference, and it is the only strong reference to a behaviour that the engine's side holds.
// `destroy` takes it back with `takeRetainedValue()`, which balances it. `fixed_update` and every
// other callback use `takeUnretainedValue()`, because they are called *while* the engine holds that
// reference and retaining again would only make a leak possible. That is `swift-scripting`'s "the
// engine SHALL hold behaviours weakly from the entity side" expressed in the only way the ABI can:
// the engine holds a pointer it never dereferences, and Swift's ARC holds the object.
//
// --- WHY THE REGISTRATION RECORD IS RETAINED FOREVER ----------------------------------------------
//
// `user_data` is a pointer the engine keeps for as long as the registration exists, and under the
// reload model a module image is NEVER unloaded — `cy/abi/module.h` explains why, at length, and it
// is a measurement rather than a preference. So the record is `passRetained` once and never
// released: releasing it would be releasing something the engine may still call through, and there
// is no later moment at which that becomes safe. The cost is one small allocation per behaviour type
// per generation, which is the same order as the 58-85 kB of address space each generation already
// costs.
//
// --- WHY EVERY THUNK CATCHES -----------------------------------------------------------------------
//
// A Swift error crossing a C function pointer is undefined; `-fno-exceptions` on the other side
// means there is no unwinder to meet it. So every thunk is total: it catches, it logs with the
// behaviour's name and the callback's, it disables the instance, and it returns a value the engine
// understands. `swift-scripting`: "SHALL disable that behaviour rather than terminating the
// process, in development builds."

import CyberdyneABI
import CyberdyneCore

/// One registered behaviour type, as this image knows it.
///
/// A class rather than a struct so that `Unmanaged` can carry it through `user_data`, and `final`
/// so that every call through it is direct.
final class BehaviourRegistration {
    let name: String
    let schema: UInt32
    let callbacks: CallbackSet
    let make: (Entity) -> any BehaviourClass

    init(name: String, schema: UInt32, callbacks: CallbackSet,
         make: @escaping (Entity) -> any BehaviourClass) {
        self.name = name
        self.schema = schema
        self.callbacks = callbacks
        self.make = make
    }

    /// Report a callback that threw, and take the instance out of dispatch.
    func report(_ error: Error, in callback: String, on instance: Behaviour) {
        instance.isEnabled = false
        Log.error("\(name).\(callback) failed: \(error). The instance is disabled; the engine keeps "
            + "running.")
    }
}

/// Registering Swift behaviour types with the engine.
public enum Behaviours {
    /// Every type registered by this image, in registration order. Held so that a diagnostic can
    /// list them and so that a test can assert what a module registered without asking the engine.
    public nonisolated(unsafe) private(set) static var registered: [String] = []

    /// Register one behaviour class.
    ///
    /// Called at `CY_INIT_LEVEL_SCENE`, which is where `native-abi` puts type registration and where
    /// a world exists to register against. Registering the same name twice in one generation
    /// replaces the vtable, which is the engine's rule (`CyEngine_T::register_behaviour`) and is
    /// what makes a reloaded module re-registering its types idempotent rather than an error.
    @discardableResult
    public static func register(_ type: any BehaviourClass.Type) throws -> BehaviourType {
        guard let interface = Runtime.interface, let engine = Runtime.engineHandle else {
            throw CyberdyneError.invalidHandle
        }
        let record = BehaviourRegistration(name: type.behaviourName, schema: type.behaviourSchema,
                                           callbacks: type.behaviourCallbacks,
                                           make: { type.init(entity: $0) })
        var vtable = makeVTable(record)
        // The name is RETAINED, not borrowed: the host keeps the pointer. See CStrings.swift for
        // what the obvious `withCString` version did instead.
        let handle = interface.registerBehaviour(engine: engine,
                                                 name: RetainedCString.make(type.behaviourName),
                                                 vtable: &vtable)
        guard let handle else {
            throw CyberdyneError.fromLastError(.internal)
        }
        registered.append(type.behaviourName)
        return BehaviourType(handle, interface)
    }

    /// Forget what this image registered. Called from `cy_module_shutdown`; the engine drops its own
    /// copy of the vtables at the same point.
    static func forgetRegistrations() {
        registered.removeAll()
    }
}

// --- The vtable ------------------------------------------------------------------------------------

private func makeVTable(_ record: BehaviourRegistration) -> CyBehaviourVTable {
    var vtable = CyBehaviourVTable()
    vtable.struct_size = UInt32(MemoryLayout<CyBehaviourVTable>.size)
    vtable.schema_version = record.schema
    vtable.create = behaviourCreate
    vtable.destroy = behaviourDestroy
    vtable.fixed_update = behaviourFixedUpdate
    vtable.serialize = behaviourSerialize
    vtable.deserialize = behaviourDeserialize
    // Retained for the life of the image. See the header comment; there is no later safe release.
    vtable.user_data = Unmanaged.passRetained(record).toOpaque()
    return vtable
}

private func registration(_ userData: UnsafeMutableRawPointer?) -> BehaviourRegistration? {
    guard let userData else { return nil }
    return Unmanaged<BehaviourRegistration>.fromOpaque(userData).takeUnretainedValue()
}

private func instance(_ raw: CyInstance?) -> (any BehaviourClass)? {
    guard let raw else { return nil }
    return Unmanaged<Behaviour>.fromOpaque(raw).takeUnretainedValue() as? any BehaviourClass
}

private let behaviourCreate: @convention(c) (CyEngine?, CyEntity, UnsafeMutableRawPointer?)
    -> CyInstance? = { _, entity, userData in
        guard let record = registration(userData) else { return nil }
        let object = record.make(Entity(bits: entity))
        if record.callbacks.contains(.create) {
            do {
                try object.onCreate()
            } catch {
                record.report(error, in: "onCreate", on: object)
            }
        }
        // Retained as `Behaviour` rather than as `any BehaviourClass`: `Unmanaged` needs a class
        // type, and the existential is not one. `destroy` takes it back at the same type, so the
        // pair balances; the concrete class is reached again through `instance(_:)`.
        let retained: Behaviour = object
        return Unmanaged.passRetained(retained).toOpaque()
    }

private let behaviourDestroy: @convention(c) (CyInstance?, UnsafeMutableRawPointer?)
    -> Void = { raw, userData in
        guard let raw, let record = registration(userData) else { return }
        let object = Unmanaged<Behaviour>.fromOpaque(raw).takeRetainedValue()
        if record.callbacks.contains(.destroy), object.isEnabled {
            do {
                try object.onDestroy()
            } catch {
                record.report(error, in: "onDestroy", on: object)
            }
        }
    }

private let behaviourFixedUpdate: @convention(c) (CyInstance?, Float, UnsafeMutableRawPointer?)
    -> Void = { raw, delta, userData in
        guard let record = registration(userData), let object = instance(raw) else { return }
        guard object.isEnabled, record.callbacks.contains(.fixedUpdate) else { return }
        do {
            try object.onFixedUpdate(Double(delta))
        } catch {
            record.report(error, in: "onFixedUpdate", on: object)
        }
    }

/// `serialize(self, NULL, 0, ud)` returns the byte count required and writes nothing; that is how
/// the host sizes the blob, and it is why this builds the blob before it looks at `capacity`.
private let behaviourSerialize: @convention(c)
    (CyInstance?, UnsafeMutablePointer<UInt8>?, UInt32, UnsafeMutableRawPointer?)
    -> UInt32 = { raw, buffer, capacity, userData in
        guard let record = registration(userData), let object = instance(raw) else { return 0 }
        var writer = BlobWriter(schema: record.schema)
        for (name, value) in object.exportedValues() {
            writer.write(name, value)
        }
        let bytes = writer.finish()
        let required = UInt32(bytes.count)
        guard let buffer, capacity >= required else { return required }
        buffer.update(from: bytes, count: bytes.count)
        return required
    }

private let behaviourDeserialize: @convention(c)
    (CyInstance?, UnsafePointer<UInt8>?, UInt32, UInt32, UnsafeMutableRawPointer?)
    -> Int32 = { raw, buffer, size, fromSchema, userData in
        guard let record = registration(userData), let object = instance(raw), let buffer else {
            return CY_RESULT_INVALID_ARGUMENT.rawValue32
        }
        // THE CHECK THE LOADER RELIES ON. A blob written by a schema newer than this code is not a
        // migration this code can perform; saying so is what makes the reload be rejected with the
        // previous generation kept live. `native-abi`'s "Incompatible reload".
        if fromSchema > record.schema {
            return CY_RESULT_SCHEMA_TOO_NEW.rawValue32
        }
        do {
            let restored = try restore(object, buffer, size, record)
            if record.callbacks.contains(.afterReload) {
                try object.onAfterReload(restored: restored)
            }
            return CY_RESULT_OK.rawValue32
        } catch let error as BlobError {
            Log.error("\(record.name): the saved state could not be read (\(error))")
            return CY_RESULT_INVALID_ARGUMENT.rawValue32
        } catch {
            record.report(error, in: "onAfterReload", on: object)
            return CY_RESULT_OK.rawValue32
        }
    }

/// Restore by name, returning the names that were actually found.
///
/// A key this class no longer has is skipped, and a property the blob does not carry keeps the
/// default its declaration gave it — which is the whole migration story, and the only one that
/// works. See Serialization.swift, item 1.
private func restore(_ object: any BehaviourClass, _ buffer: UnsafePointer<UInt8>, _ size: UInt32,
                     _ record: BehaviourRegistration) throws -> Set<String> {
    var reader = try BlobReader(UnsafeRawBufferPointer(start: buffer, count: Int(size)))
    var restored: Set<String> = []
    while let entry = try reader.next() {
        guard let storage = object.exportedStorage(named: entry.key) else {
            // A key this class no longer has. `onMigrate` is the one chance to claim it — a rename
            // is otherwise a silent loss, because by-name restore cannot see that two names mean
            // the same field.
            if record.callbacks.contains(.migrate), try object.onMigrate(entry.key, entry.value) {
                restored.insert(entry.key)
            }
            continue
        }
        if storage.assign(entry.value) {
            restored.insert(entry.key)
        } else {
            Log.warning("\(record.name).\(entry.key) changed type between schemas; the saved value "
                + "was dropped and the property keeps its default.")
        }
    }
    return restored
}

extension CyResult {
    /// `CyBehaviourVTable.deserialize` returns `int32_t`, and `CyResult`'s raw type is unsigned.
    /// Named rather than open-coded at three call sites so the conversion is stated once.
    var rawValue32: Int32 { Int32(bitPattern: UInt32(rawValue)) }
}

// --- The tree callbacks, which the ABI cannot drive yet ----------------------------------------------

extension Behaviour {
    /// Drive one lifecycle callback by hand, with the same guarantees the engine's own thunks give:
    /// nothing is called on a class that did not write it, nothing is called on a disabled instance,
    /// and a thrown error disables the instance rather than escaping into C.
    ///
    /// WHY THIS IS PUBLIC AND WHY IT IS NOT A WORKAROUND. `CyBehaviourVTable` carries `create`,
    /// `destroy`, `fixed_update`, `serialize` and `deserialize`. The tree callbacks
    /// `scene-graph-and-nodes` defines — `onEnterTree`, `onReady`, `onEnable`, `onDisable`,
    /// `onUpdate`, `onExitTree` — need scene and frame entries that ABI 1.0's table does not have.
    /// They are part of the model, they are recorded in `behaviourCallbacks`, and this is the one
    /// place that dispatches them. When those entries are APPENDED to `CyInterface` — the only legal
    /// way to grow it — the new thunks call exactly this, and nothing in a game changes.
    ///
    /// `delta` is ignored by every callback that does not take one.
    public func dispatch(_ callback: CallbackSet, delta: Double = 0) {
        guard isEnabled, let typed = self as? any BehaviourClass,
              type(of: typed).behaviourCallbacks.contains(callback) else { return }
        do {
            try invoke(callback, delta)
        } catch {
            isEnabled = false
            let name = callback.names.first ?? "a callback"
            Log.error("\(type(of: typed).behaviourName).\(name) failed: \(error). The instance is "
                + "disabled; the engine keeps running.")
        }
    }

    private func invoke(_ callback: CallbackSet, _ delta: Double) throws {
        switch callback {
        case .create: try onCreate()
        case .enterTree: try onEnterTree()
        case .ready: try onReady()
        case .enable: try onEnable()
        case .disable: try onDisable()
        case .fixedUpdate: try onFixedUpdate(delta)
        case .update: try onUpdate(delta)
        case .exitTree: try onExitTree()
        case .destroy: try onDestroy()
        default:
            // `.afterReload` and `.migrate` take arguments this cannot supply, and a set with more
            // than one member names no single callback. Both are programmer error rather than a
            // runtime condition, so they are reported and ignored — a module that traps takes the
            // engine's process with it.
            Log.warning("dispatch(\(callback.names)) is not a single argument-free callback")
        }
    }
}
