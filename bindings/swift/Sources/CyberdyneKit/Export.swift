// Export.swift — `@Export` and `@Node`, the two property wrappers a behaviour declares state with.
// Tasks 3.2, 3.4, 3.5.
//
// WHY THESE ARE PROPERTY WRAPPERS AND THE REGISTRATION IS A MACRO.
//
// A macro rewrites source; a property wrapper carries behaviour and storage. `@Export` needs both:
// a value that lives in the instance and reads and writes like a plain `var`, and a name the
// serializer can find it by. The wrapper gives the first, and the `@Behaviour` macro — which sees
// the whole class body — gives the second, by emitting a by-name accessor over exactly the
// properties that carry `@Export`. Neither can do the other's half: a wrapper cannot see its own
// declared name, and a macro cannot hold storage.
//
// `swift-scripting`: "@Export — marks a property for inspector, serialization, and (optionally)
// replication" and "it SHALL appear in the editor inspector with its type and constraints, be
// serialized with the scene, and be settable per instance". The inspector is M5's; what lands here
// is the type, the constraints, and the serialization.

import CyberdyneCore

/// A constraint an exported property carries into the inspector and into validation.
public enum ExportConstraint: Equatable, Sendable {
    case none
    /// A closed numeric range. Held as `Double` so one case covers `Float`, `Double` and the
    /// integers rather than three near-identical ones.
    case range(ClosedRange<Double>)
}

/// The type-erased face of an `@Export`ed property: what the serializer and the inspector see.
///
/// It is a class-bound existential because the wrapper's storage must be mutated through it. The
/// `@Behaviour` macro emits a switch that returns the concrete wrapper, so nothing here ever has to
/// look a property up by reflection — which matters more than it looks: `Mirror` and
/// `_typeByName` are the process-global, first-registration-wins lookups the hot-reload spike
/// measured returning a RETIRED generation's metadata. This package uses neither.
public protocol ExportedStorage: AnyObject {
    var exportedValue: Value { get }
    var exportedConstraint: ExportConstraint { get }
    /// Assign from a value read back out of a blob. False when the kinds do not match, which is a
    /// field whose type changed between schemas: the field keeps its default and the behaviour is
    /// told, rather than being handed reinterpreted bits.
    func assign(_ value: Value) -> Bool
}

/// An inspectable, serializable property of a behaviour.
@propertyWrapper
public final class Export<T: Exportable>: ExportedStorage {
    public var wrappedValue: T
    public let exportedConstraint: ExportConstraint

    public init(wrappedValue: T) {
        self.wrappedValue = wrappedValue
        exportedConstraint = .none
    }

    /// `@Export(range: 0...20) var jumpVelocity: Float = 8.0` — the spelling `swift-scripting` uses.
    public init(wrappedValue: T, range: ClosedRange<Double>) {
        self.wrappedValue = wrappedValue
        exportedConstraint = .range(range)
    }

    /// The wrapper itself, so `_speed` reaches the storage and `$speed` reaches the projection.
    public var projectedValue: Export<T> { self }

    public var exportedValue: Value { wrappedValue.cyValue }

    public func assign(_ value: Value) -> Bool {
        guard let restored = T(cyValue: value) else { return false }
        wrappedValue = restored
        return true
    }
}

/// A node reference resolved by path.
///
/// `swift-scripting`: "it SHALL be resolved at `onReady` and be `nil` if the path does not resolve,
/// rather than trapping". The wrapper is therefore an Optional that starts nil and is filled in by
/// resolution, and reading it before `onReady` is nil rather than a trap — which is the same
/// answer as a path that does not exist, deliberately: a behaviour that handles one handles both.
///
/// WHAT IS NOT HERE YET, STATED AS IT IS RATHER THAN AS IT WILL BE. Resolution needs the scene's
/// path lookup, and ABI 1.0's table has no node entry — `CyInterface` carries the world, components
/// and behaviours and nothing about the node graph. So `resolve(_:)` is the seam and **nothing
/// calls it**: no resolver exists, every `@Node` reads nil for the process's life, and there is no
/// warning, because a warning would need a resolution attempt to report the failure of. When
/// `scene-graph-and-nodes`' entries are appended, the bridge resolves at `onReady` and an
/// unresolved path becomes a diagnostic there; the wrapper does not change.
@propertyWrapper
public final class Node<T> {
    public let path: String
    public private(set) var wrappedValue: T?

    public init(_ path: String) {
        self.path = path
        wrappedValue = nil
    }

    public var projectedValue: Node<T> { self }

    /// Fill in the reference. The behaviour bridge will call this at `onReady` once the node
    /// entries exist; today only a test does, which is what keeps the wrapper exercised.
    public func resolve(_ value: T?) {
        wrappedValue = value
    }
}
