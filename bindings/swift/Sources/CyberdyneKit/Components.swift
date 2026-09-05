// Components.swift — Swift structs as ECS components, and the typed fast paths. Tasks 3.2, 3.3.
//
// `native-abi` requires typed accessors "so that a behaviour updating a transform every tick does
// not marshal": `component_get_f32`, `component_set_f32`, `component_get_vec3`,
// `component_set_vec3` read and write the field's bytes after one type check, and the generated
// overlay exposes them on `World`. This file is the ergonomic face of those — a `Float` returned
// rather than an out-parameter filled — and the registration that gives a Swift struct a place in
// the world to be read out of.
//
// THE GENERIC PATH IS STILL HERE AND IS STILL CORRECT. `value(_:)` and `setValue(_:)` go through
// `CyVar`, which is what a tool, an inspector or anything reflective wants. The rule is the one
// cy_abi.h states: the typed entries for a hot loop, the `CyVar` path for everything else.

import CyberdyneABI
import CyberdyneCore

/// One field of a component, as the engine is told about it.
public struct FieldDescriptor: Sendable, Equatable {
    public let name: String
    public let type: VarType
    public let offset: Int
    public let size: Int

    public init(name: String, type: VarType, offset: Int, size: Int) {
        self.name = name
        self.type = type
        self.offset = offset
        self.size = size
    }
}

/// A Swift value type the engine stores in chunk memory.
///
/// `swift-scripting` requires the `@Component` macro to reject a struct containing a class
/// reference at compile time, "explaining that components must be trivially relocatable value
/// types". This protocol is what the rejection defends: conformance means the layout is the
/// engine's to move, and a type holding a reference would have its refcount moved with `memcpy` and
/// its object leaked or freed twice.
public protocol Component {
    /// The name the engine registers this component under. Registration is idempotent by name, so a
    /// reloaded module re-registering its components gets the same ids back.
    static var componentName: String { get }
    /// The fields, in declaration order. Field index is the order of this array, and it is what
    /// every typed accessor takes.
    static var componentFields: [FieldDescriptor] { get }

    init()
}

/// Registering Swift components with the world.
public enum Components {
    /// Register a component type, or return the id an equal earlier registration got.
    public static func register(_ type: any Component.Type, in world: World) throws -> ComponentType {
        let fields = type.componentFields
        // The names must outlive the registration: cy_abi.h says the engine stores the pointer.
        // See CStrings.swift, which exists because the obvious `withCString` spelling of this
        // silently registers a name that is freed before anyone looks it up.
        let names = fields.map { RetainedCString.make($0.name) }
        let componentName = RetainedCString.make(type.componentName)
        var descriptors: [CyFieldDesc] = []
        descriptors.reserveCapacity(fields.count)
        for (index, field) in fields.enumerated() {
            var descriptor = CyFieldDesc()
            descriptor.struct_size = UInt32(MemoryLayout<CyFieldDesc>.size)
            descriptor.type = field.type.rawValue
            descriptor.offset = UInt32(field.offset)
            descriptor.size = UInt32(field.size)
            descriptor.name = names[index]
            descriptors.append(descriptor)
        }

        let id = descriptors.withUnsafeBufferPointer { buffer -> CyComponentTypeId in
            var desc = CyComponentTypeDesc()
            desc.struct_size = UInt32(MemoryLayout<CyComponentTypeDesc>.size)
            desc.size = UInt32(componentStride(type))
            desc.alignment = UInt32(componentAlignment(type))
            desc.field_count = UInt32(fields.count)
            desc.name = componentName
            desc.fields = buffer.baseAddress
            return world.registerComponent(desc: &desc)
        }
        let component = ComponentType(id: id)
        guard component.isValid else { throw CyberdyneError.fromLastError(.internal) }
        return component
    }
}

/// A component's byte size and alignment, as the engine must be told them.
///
/// Two free functions rather than protocol requirements, because `MemoryLayout` already knows and a
/// protocol requirement would be an opportunity to state a different number. Opening the existential
/// is what lets `MemoryLayout` see the concrete type.
private func componentStride(_ type: any Component.Type) -> Int {
    func size<T>(_: T.Type) -> Int { MemoryLayout<T>.stride }
    return _openExistential(type, do: size)
}

private func componentAlignment(_ type: any Component.Type) -> Int {
    func alignment<T>(_: T.Type) -> Int { MemoryLayout<T>.alignment }
    return _openExistential(type, do: alignment)
}

// --- The ergonomic face of the typed fast paths -----------------------------------------------------

extension World {
    /// One `Float` field, through `component_get_f32` — no `CyVar`, one type check, the field's own
    /// bytes.
    public func float(_ entity: Entity, _ component: ComponentType, field: UInt32) throws -> Float {
        var value: Float = 0
        try componentGetF32(entity: entity.bits, component: component.id, field: field, into: &value)
        return value
    }

    public func setFloat(_ value: Float, _ entity: Entity, _ component: ComponentType,
                         field: UInt32) throws {
        try componentSetF32(entity: entity.bits, component: component.id, field: field, value: value)
    }

    public func vec3(_ entity: Entity, _ component: ComponentType, field: UInt32) throws -> Vec3 {
        var lanes = (Float(0), Float(0), Float(0))
        try withUnsafeMutablePointer(to: &lanes) { tuple in
            try tuple.withMemoryRebound(to: Float.self, capacity: 3) { storage in
                try componentGetVec3(entity: entity.bits, component: component.id, field: field,
                                     into: storage)
            }
        }
        return Vec3(x: lanes.0, y: lanes.1, z: lanes.2)
    }

    public func setVec3(_ value: Vec3, _ entity: Entity, _ component: ComponentType,
                        field: UInt32) throws {
        var lanes = (value.x, value.y, value.z)
        try withUnsafeMutablePointer(to: &lanes) { tuple in
            try tuple.withMemoryRebound(to: Float.self, capacity: 3) { storage in
                try componentSetVec3(entity: entity.bits, component: component.id, field: field,
                                     xyz: storage)
            }
        }
    }

    /// The generic, reflective path. Correct for tools; not for a hot loop.
    public func value(_ entity: Entity, _ component: ComponentType, field: UInt32) throws -> Value {
        var variable = CyVar()
        try componentGetVar(entity: entity.bits, component: component.id, field: field,
                            into: &variable)
        defer { interface.varRelease(value: &variable) }
        guard let value = Value(reading: variable) else {
            throw CyberdyneError.notRepresentable("CyVarType \(variable.type)")
        }
        return value
    }

    public func setValue(_ value: Value, _ entity: Entity, _ component: ComponentType,
                         field: UInt32) throws {
        try value.withCyVar { variable in
            var copy = variable
            try componentSetVar(entity: entity.bits, component: component.id, field: field,
                                value: &copy)
        }
    }

    // --- entities -------------------------------------------------------------------------------

    /// A new entity, or `CyberdyneError` carrying the engine's own message.
    public func makeEntity() throws -> Entity {
        let entity = Entity(bits: createEntity())
        guard !entity.isNull else { throw CyberdyneError.fromLastError(.internal) }
        return entity
    }

    public func destroy(_ entity: Entity) throws {
        try destroyEntity(entity: entity.bits)
    }

    /// `swift-scripting`: "Accessing a handle whose target is destroyed SHALL return `nil` or throw,
    /// never trap on freed memory." This is the question that makes that possible to ask.
    public func isAlive(_ entity: Entity) -> Bool {
        entityAlive(entity: entity.bits)
    }

    public func add(_ component: ComponentType, to entity: Entity) throws {
        try addComponent(entity: entity.bits, component: component.id, initial: nil)
    }

    public func add<T: Component>(_ value: T, as component: ComponentType, to entity: Entity) throws {
        var copy = value
        try withUnsafeBytes(of: &copy) { bytes in
            try addComponent(entity: entity.bits, component: component.id,
                             initial: bytes.baseAddress)
        }
    }

    public func remove(_ component: ComponentType, from entity: Entity) throws {
        try removeComponent(entity: entity.bits, component: component.id)
    }

    public func has(_ component: ComponentType, on entity: Entity) -> Bool {
        hasComponent(entity: entity.bits, component: component.id)
    }

    public func find(component name: String) -> ComponentType {
        ComponentType(id: name.withCString { findComponent(name: $0) })
    }
}
