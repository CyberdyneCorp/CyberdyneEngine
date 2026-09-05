// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/swift/overlay_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just generate-swift`, and `just generate-swift --check` fails when this file is stale.

import CyberdyneABI

/// The engine's exported interface table, with one Swift method per entry.
///
/// It is deliberately thin: every method is the imported C call and nothing else, so there is no
/// second model of the ABI anywhere in this package. The ergonomics — Swift `String`s, `Vec3`,
/// component accessors that return rather than fill an out-parameter — are `CyberdyneKit`'s, which
/// is hand-written on purpose (`swift-scripting` names it "the hand-written ergonomic layer").
///
/// `@unchecked Sendable` because it is a pointer to memory the engine owns for the process
/// lifetime and never mutates after `cy_get_interface` returns. The rule that actually governs
/// which thread may call these is in `CyberdyneKit/Concurrency.swift`, and it is stronger than
/// `Sendable`: engine mutation is confined to `@GameActor`.
public struct Interface: @unchecked Sendable {
    public let table: UnsafePointer<CyInterface>

    @inlinable
    public init(_ table: UnsafePointer<CyInterface>) {
        self.table = table
    }

    /// The table the engine actually exported. `native-abi`'s additive growth rule from this side:
    /// the engine may export MORE entries than this overlay knows, never fewer.
    @inlinable public var abiMajor: UInt32 { table.pointee.header.abi_major }
    @inlinable public var abiMinor: UInt32 { table.pointee.header.abi_minor }
    @inlinable public var abiPatch: UInt32 { table.pointee.header.abi_patch }
    @inlinable public var tableSize: UInt32 { table.pointee.header.table_size }

    /// The module's half of the version handshake, `native-abi`'s "Older engine, newer module".
    ///
    /// True when this overlay may call every entry it knows about. A module that gets `false` must
    /// return `false` from `cy_module_entry` — which the loader reports and survives — rather than
    /// calling into a table that stops short of what it was compiled against.
    @inlinable
    public var isCompatible: Bool {
        abiMajor == ABI.major && tableSize >= ABI.interfaceTableSize
    }

    /// Turn a failing `CyResult` into a thrown `CyberdyneError`, carrying the engine's message.
    ///
    /// The message is copied here rather than held: `get_last_error` documents its pointer as valid
    /// only until this thread's next failing call, so a `String` made later would read whatever
    /// failed since.
    @inlinable
    public func check(_ result: CyResult) throws {
        if result == CY_RESULT_OK { return }
        let status = Status(rawValue: Int32(result.rawValue)) ?? .unknown
        var message = ""
        if let text = table.pointee.get_last_error() {
            message = String(cString: text)
        }
        throw CyberdyneError.status(status, message: message)
    }

    @inlinable
    public func log(engine: CyEngine, severity: UInt32, message: UnsafePointer<CChar>?) {
        table.pointee.log(engine, severity, message)
    }

    @inlinable
    public func getLastError() -> UnsafePointer<CChar>? {
        table.pointee.get_last_error()
    }

    @inlinable
    public func getLastErrorCode() -> CyResult {
        table.pointee.get_last_error_code()
    }

    @inlinable
    public func setLastError(result: CyResult, message: UnsafePointer<CChar>?) {
        table.pointee.set_last_error(result, message)
    }

    @inlinable
    public func varMakeString(engine: CyEngine, utf8: UnsafePointer<CChar>?, length: UInt64) -> CyVar {
        table.pointee.var_make_string(engine, utf8, length)
    }

    @inlinable
    public func varMakeBytes(engine: CyEngine, data: UnsafeRawPointer?, size: UInt64) -> CyVar {
        table.pointee.var_make_bytes(engine, data, size)
    }

    @inlinable
    public func varClone(value: UnsafePointer<CyVar>?) -> CyVar {
        table.pointee.var_clone(value)
    }

    @inlinable
    public func varRelease(value: UnsafeMutablePointer<CyVar>?) {
        table.pointee.var_release(value)
    }

    @inlinable
    public func varLiveCount(engine: CyEngine) -> UInt64 {
        table.pointee.var_live_count(engine)
    }

    @inlinable
    public func engineWorld(engine: CyEngine) -> CyWorld? {
        table.pointee.engine_world(engine)
    }

    @inlinable
    public func worldCreateEntity(world: CyWorld) -> CyEntity {
        table.pointee.world_create_entity(world)
    }

    @inlinable
    public func worldDestroyEntity(world: CyWorld, entity: CyEntity) throws {
        try check(table.pointee.world_destroy_entity(world, entity))
    }

    @inlinable
    public func worldEntityAlive(world: CyWorld, entity: CyEntity) -> Bool {
        table.pointee.world_entity_alive(world, entity)
    }

    @inlinable
    public func worldEpoch(world: CyWorld) -> UInt64 {
        table.pointee.world_epoch(world)
    }

    @inlinable
    public func worldRegisterComponent(world: CyWorld, desc: UnsafePointer<CyComponentTypeDesc>?) -> CyComponentTypeId {
        table.pointee.world_register_component(world, desc)
    }

    @inlinable
    public func worldFindComponent(world: CyWorld, name: UnsafePointer<CChar>?) -> CyComponentTypeId {
        table.pointee.world_find_component(world, name)
    }

    @inlinable
    public func worldAddComponent(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, initial: UnsafeRawPointer?) throws {
        try check(table.pointee.world_add_component(world, entity, component, initial))
    }

    @inlinable
    public func worldRemoveComponent(world: CyWorld, entity: CyEntity, component: CyComponentTypeId) throws {
        try check(table.pointee.world_remove_component(world, entity, component))
    }

    @inlinable
    public func worldHasComponent(world: CyWorld, entity: CyEntity, component: CyComponentTypeId) -> Bool {
        table.pointee.world_has_component(world, entity, component)
    }

    @inlinable
    public func worldBorrowComponent(world: CyWorld, entity: CyEntity, component: CyComponentTypeId) -> CyBorrow {
        table.pointee.world_borrow_component(world, entity, component)
    }

    @inlinable
    public func borrowValid(world: CyWorld, borrow: CyBorrow) -> Bool {
        table.pointee.borrow_valid(world, borrow)
    }

    @inlinable
    public func componentGetVar(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, field: UInt32, into: UnsafeMutablePointer<CyVar>?) throws {
        try check(table.pointee.component_get_var(world, entity, component, field, into))
    }

    @inlinable
    public func componentSetVar(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, field: UInt32, value: UnsafePointer<CyVar>?) throws {
        try check(table.pointee.component_set_var(world, entity, component, field, value))
    }

    @inlinable
    public func componentGetF32(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, field: UInt32, into: UnsafeMutablePointer<Float>?) throws {
        try check(table.pointee.component_get_f32(world, entity, component, field, into))
    }

    @inlinable
    public func componentSetF32(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, field: UInt32, value: Float) throws {
        try check(table.pointee.component_set_f32(world, entity, component, field, value))
    }

    @inlinable
    public func componentGetVec3(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, field: UInt32, into: UnsafeMutablePointer<Float>?) throws {
        try check(table.pointee.component_get_vec3(world, entity, component, field, into))
    }

    @inlinable
    public func componentSetVec3(world: CyWorld, entity: CyEntity, component: CyComponentTypeId, field: UInt32, xyz: UnsafePointer<Float>?) throws {
        try check(table.pointee.component_set_vec3(world, entity, component, field, xyz))
    }

    @inlinable
    public func registerBehaviour(engine: CyEngine, name: UnsafePointer<CChar>?, vtable: UnsafePointer<CyBehaviourVTable>?) -> CyBehaviourType? {
        table.pointee.register_behaviour(engine, name, vtable)
    }

    @inlinable
    public func findBehaviour(engine: CyEngine, name: UnsafePointer<CChar>?) -> CyBehaviourType? {
        table.pointee.find_behaviour(engine, name)
    }

    @inlinable
    public func behaviourGeneration(type: CyBehaviourType) -> UInt32 {
        table.pointee.behaviour_generation(type)
    }
}
