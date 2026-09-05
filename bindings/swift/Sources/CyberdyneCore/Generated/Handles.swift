// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/swift/overlay_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just generate-swift`, and `just generate-swift --check` fails when this file is stale.

import CyberdyneABI

// The typed wrappers over the ABI's opaque handles.
//
// Which entries land on which wrapper is DERIVED rather than listed: an entry whose first parameter
// is a `CyWorld` is a method on `World`, and one whose first parameter is not a handle at all stays
// on `Interface`. So appending a `world_*` entry to `CyInterface` puts a method on `World` with no
// edit here, and appending one that takes no handle does not.
//
// A wrapper is a handle plus the table to reach it through; it owns nothing and keeps nothing
// alive. `swift-scripting`: "A `Node` or entity wrapper is a **handle**, not an owning reference;
// holding one does not keep the entity alive."

public struct BehaviourType: @unchecked Sendable {
    public let raw: CyBehaviourType
    public let interface: Interface

    @inlinable
    public init(_ raw: CyBehaviourType, _ interface: Interface) {
        self.raw = raw
        self.interface = interface
    }

    @inlinable
    public func generation() -> UInt32 {
        interface.behaviourGeneration(type: raw)
    }
}

public struct Engine: @unchecked Sendable {
    public let raw: CyEngine
    public let interface: Interface

    @inlinable
    public init(_ raw: CyEngine, _ interface: Interface) {
        self.raw = raw
        self.interface = interface
    }

    @inlinable
    public func log(severity: UInt32, message: UnsafePointer<CChar>?) {
        interface.log(engine: raw, severity: severity, message: message)
    }

    @inlinable
    public func varMakeString(utf8: UnsafePointer<CChar>?, length: UInt64) -> CyVar {
        interface.varMakeString(engine: raw, utf8: utf8, length: length)
    }

    @inlinable
    public func varMakeBytes(data: UnsafeRawPointer?, size: UInt64) -> CyVar {
        interface.varMakeBytes(engine: raw, data: data, size: size)
    }

    @inlinable
    public func varLiveCount() -> UInt64 {
        interface.varLiveCount(engine: raw)
    }

    @inlinable
    public func world() -> CyWorld? {
        interface.engineWorld(engine: raw)
    }

    @inlinable
    public func registerBehaviour(name: UnsafePointer<CChar>?, vtable: UnsafePointer<CyBehaviourVTable>?) -> CyBehaviourType? {
        interface.registerBehaviour(engine: raw, name: name, vtable: vtable)
    }

    @inlinable
    public func findBehaviour(name: UnsafePointer<CChar>?) -> CyBehaviourType? {
        interface.findBehaviour(engine: raw, name: name)
    }
}

public struct World: @unchecked Sendable {
    public let raw: CyWorld
    public let interface: Interface

    @inlinable
    public init(_ raw: CyWorld, _ interface: Interface) {
        self.raw = raw
        self.interface = interface
    }

    @inlinable
    public func createEntity() -> CyEntity {
        interface.worldCreateEntity(world: raw)
    }

    @inlinable
    public func destroyEntity(entity: CyEntity) throws {
        try interface.worldDestroyEntity(world: raw, entity: entity)
    }

    @inlinable
    public func entityAlive(entity: CyEntity) -> Bool {
        interface.worldEntityAlive(world: raw, entity: entity)
    }

    @inlinable
    public func epoch() -> UInt64 {
        interface.worldEpoch(world: raw)
    }

    @inlinable
    public func registerComponent(desc: UnsafePointer<CyComponentTypeDesc>?) -> CyComponentTypeId {
        interface.worldRegisterComponent(world: raw, desc: desc)
    }

    @inlinable
    public func findComponent(name: UnsafePointer<CChar>?) -> CyComponentTypeId {
        interface.worldFindComponent(world: raw, name: name)
    }

    @inlinable
    public func addComponent(entity: CyEntity, component: CyComponentTypeId, initial: UnsafeRawPointer?) throws {
        try interface.worldAddComponent(world: raw, entity: entity, component: component, initial: initial)
    }

    @inlinable
    public func removeComponent(entity: CyEntity, component: CyComponentTypeId) throws {
        try interface.worldRemoveComponent(world: raw, entity: entity, component: component)
    }

    @inlinable
    public func hasComponent(entity: CyEntity, component: CyComponentTypeId) -> Bool {
        interface.worldHasComponent(world: raw, entity: entity, component: component)
    }

    @inlinable
    public func borrowComponent(entity: CyEntity, component: CyComponentTypeId) -> CyBorrow {
        interface.worldBorrowComponent(world: raw, entity: entity, component: component)
    }

    @inlinable
    public func borrowValid(borrow: CyBorrow) -> Bool {
        interface.borrowValid(world: raw, borrow: borrow)
    }

    @inlinable
    public func componentGetVar(entity: CyEntity, component: CyComponentTypeId, field: UInt32, into: UnsafeMutablePointer<CyVar>?) throws {
        try interface.componentGetVar(world: raw, entity: entity, component: component, field: field, into: into)
    }

    @inlinable
    public func componentSetVar(entity: CyEntity, component: CyComponentTypeId, field: UInt32, value: UnsafePointer<CyVar>?) throws {
        try interface.componentSetVar(world: raw, entity: entity, component: component, field: field, value: value)
    }

    @inlinable
    public func componentGetF32(entity: CyEntity, component: CyComponentTypeId, field: UInt32, into: UnsafeMutablePointer<Float>?) throws {
        try interface.componentGetF32(world: raw, entity: entity, component: component, field: field, into: into)
    }

    @inlinable
    public func componentSetF32(entity: CyEntity, component: CyComponentTypeId, field: UInt32, value: Float) throws {
        try interface.componentSetF32(world: raw, entity: entity, component: component, field: field, value: value)
    }

    @inlinable
    public func componentGetVec3(entity: CyEntity, component: CyComponentTypeId, field: UInt32, into: UnsafeMutablePointer<Float>?) throws {
        try interface.componentGetVec3(world: raw, entity: entity, component: component, field: field, into: into)
    }

    @inlinable
    public func componentSetVec3(entity: CyEntity, component: CyComponentTypeId, field: UInt32, xyz: UnsafePointer<Float>?) throws {
        try interface.componentSetVec3(world: raw, entity: entity, component: component, field: field, xyz: xyz)
    }
}

/// An entity, as the ABI carries it. `native-abi` fixes the encoding: the 32-bit index low and the
/// 32-bit generation high, with zero reserved for the null entity because a generation of zero is
/// never issued.
@frozen
public struct Entity: Hashable, Sendable {
    public var bits: CyEntity

    @inlinable
    public init(bits: CyEntity) {
        self.bits = bits
    }

    /// `CY_ENTITY_NULL`, read from the C header rather than written as a literal here.
    public static let null = Entity(bits: CY_ENTITY_NULL)

    @inlinable public var isNull: Bool { bits == CY_ENTITY_NULL }
}

/// A component type's id within one world. Registration order is id order, so an id is meaningful
/// only against the world that issued it.
@frozen
public struct ComponentType: Hashable, Sendable {
    public var id: CyComponentTypeId

    @inlinable
    public init(id: CyComponentTypeId) {
        self.id = id
    }

    public static let invalid = ComponentType(id: CY_COMPONENT_TYPE_INVALID)

    @inlinable public var isValid: Bool { id != CY_COMPONENT_TYPE_INVALID }
}
