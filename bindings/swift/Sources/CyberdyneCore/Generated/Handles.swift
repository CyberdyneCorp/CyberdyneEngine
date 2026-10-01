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

    @inlinable
    public func serviceOpen(into: UnsafeMutablePointer<CyServiceSession?>?) throws {
        try interface.serviceOpen(engine: raw, into: into)
    }

    @inlinable
    public func serviceClose(session: CyServiceSession) {
        interface.serviceClose(engine: raw, session: session)
    }

    @inlinable
    public func serviceSubmit(session: CyServiceSession, request: UnsafePointer<CyServiceRequest>?) throws {
        try interface.serviceSubmit(engine: raw, session: session, request: request)
    }

    @inlinable
    public func serviceCancel(session: CyServiceSession, requestId: UInt64) throws {
        try interface.serviceCancel(engine: raw, session: session, requestId: requestId)
    }

    @inlinable
    public func servicePoll(session: CyServiceSession, event: UnsafeMutablePointer<CyServiceEvent>?, hasEvent: UnsafeMutablePointer<Bool>?) throws {
        try interface.servicePoll(engine: raw, session: session, event: event, hasEvent: hasEvent)
    }

    @inlinable
    public func timeGet(into: UnsafeMutablePointer<CyTime>?) throws {
        try interface.timeGet(engine: raw, into: into)
    }

    @inlinable
    public func inputFindAction(name: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyInputAction>?) throws {
        try interface.inputFindAction(engine: raw, name: name, into: into)
    }

    @inlinable
    public func inputActionState(user: UInt32, action: CyInputAction, into: UnsafeMutablePointer<CyInputActionState>?) throws {
        try interface.inputActionState(engine: raw, user: user, action: action, into: into)
    }

    @inlinable
    public func inputActionStateByName(user: UInt32, name: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyInputActionState>?) throws {
        try interface.inputActionStateByName(engine: raw, user: user, name: name, into: into)
    }

    @inlinable
    public func inputPointer(user: UInt32, into: UnsafeMutablePointer<CyInputPointer>?) throws {
        try interface.inputPointer(engine: raw, user: user, into: into)
    }

    @inlinable
    public func inputModifiers(user: UInt32, into: UnsafeMutablePointer<UInt32>?) throws {
        try interface.inputModifiers(engine: raw, user: user, into: into)
    }

    @inlinable
    public func inputFindContext(name: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyInputContext>?) throws {
        try interface.inputFindContext(engine: raw, name: name, into: into)
    }

    @inlinable
    public func inputPushContext(user: UInt32, context: CyInputContext, priority: Int32) throws {
        try interface.inputPushContext(engine: raw, user: user, context: context, priority: priority)
    }

    @inlinable
    public func inputPopContext(user: UInt32, context: CyInputContext) throws {
        try interface.inputPopContext(engine: raw, user: user, context: context)
    }

    @inlinable
    public func cameraActive(into: UnsafeMutablePointer<CyCamera>?) throws {
        try interface.cameraActive(engine: raw, into: into)
    }

    @inlinable
    public func cameraView(camera: CyCamera, into: UnsafeMutablePointer<CyCameraView>?) throws {
        try interface.cameraView(engine: raw, camera: camera, into: into)
    }

    @inlinable
    public func cameraScreenToRay(camera: CyCamera, screen: UnsafePointer<Float>?, into: UnsafeMutablePointer<CyRay>?) throws {
        try interface.cameraScreenToRay(engine: raw, camera: camera, screen: screen, into: into)
    }

    @inlinable
    public func cameraWorldToScreen(camera: CyCamera, points: UnsafePointer<Float>?, count: UInt32, into: UnsafeMutablePointer<CyScreenPoint>?) throws {
        try interface.cameraWorldToScreen(engine: raw, camera: camera, points: points, count: count, into: into)
    }

    @inlinable
    public func cameraSetTarget(camera: CyCamera, target: UnsafePointer<CyCameraTarget>?) throws {
        try interface.cameraSetTarget(engine: raw, camera: camera, target: target)
    }

    @inlinable
    public func cameraSetPose(camera: CyCamera, pose: UnsafePointer<CyPose>?) throws {
        try interface.cameraSetPose(engine: raw, camera: camera, pose: pose)
    }

    @inlinable
    public func cameraClearPose(camera: CyCamera) throws {
        try interface.cameraClearPose(engine: raw, camera: camera)
    }

    @inlinable
    public func physicsRaycast(ray: UnsafePointer<CyRay>?, filter: UnsafePointer<CyQueryFilter>?, into: UnsafeMutablePointer<CyPhysicsHit>?, hasHit: UnsafeMutablePointer<Bool>?) throws {
        try interface.physicsRaycast(engine: raw, ray: ray, filter: filter, into: into, hasHit: hasHit)
    }

    @inlinable
    public func physicsRaycastAll(ray: UnsafePointer<CyRay>?, filter: UnsafePointer<CyQueryFilter>?, into: UnsafeMutablePointer<CyPhysicsHit>?, capacity: UInt32, count: UnsafeMutablePointer<UInt32>?) throws {
        try interface.physicsRaycastAll(engine: raw, ray: ray, filter: filter, into: into, capacity: capacity, count: count)
    }

    @inlinable
    public func physicsShapeCast(shape: UnsafePointer<CyShape>?, start: UnsafePointer<CyPose>?, direction: UnsafePointer<Float>?, maxDistance: Float, filter: UnsafePointer<CyQueryFilter>?, into: UnsafeMutablePointer<CyPhysicsHit>?, hasHit: UnsafeMutablePointer<Bool>?) throws {
        try interface.physicsShapeCast(engine: raw, shape: shape, start: start, direction: direction, maxDistance: maxDistance, filter: filter, into: into, hasHit: hasHit)
    }

    @inlinable
    public func physicsOverlap(shape: UnsafePointer<CyShape>?, pose: UnsafePointer<CyPose>?, filter: UnsafePointer<CyQueryFilter>?, into: UnsafeMutablePointer<CyEntity>?, capacity: UInt32, count: UnsafeMutablePointer<UInt32>?) throws {
        try interface.physicsOverlap(engine: raw, shape: shape, pose: pose, filter: filter, into: into, capacity: capacity, count: count)
    }

    @inlinable
    public func navFindPath(request: UnsafePointer<CyNavPathRequest>?, into: UnsafeMutablePointer<Float>?, capacity: UInt32, result: UnsafeMutablePointer<CyNavPathResult>?) throws {
        try interface.navFindPath(engine: raw, request: request, into: into, capacity: capacity, result: result)
    }

    @inlinable
    public func navRequestPath(request: UnsafePointer<CyNavPathRequest>?, into: UnsafeMutablePointer<CyNavQuery>?) throws {
        try interface.navRequestPath(engine: raw, request: request, into: into)
    }

    @inlinable
    public func navPollPath(query: CyNavQuery, into: UnsafeMutablePointer<Float>?, capacity: UInt32, result: UnsafeMutablePointer<CyNavPathResult>?) throws {
        try interface.navPollPath(engine: raw, query: query, into: into, capacity: capacity, result: result)
    }

    @inlinable
    public func navCancelPath(query: CyNavQuery) throws {
        try interface.navCancelPath(engine: raw, query: query)
    }

    @inlinable
    public func navAgentConfigure(entity: CyEntity, params: UnsafePointer<CyNavAgentParams>?) throws {
        try interface.navAgentConfigure(engine: raw, entity: entity, params: params)
    }

    @inlinable
    public func navAgentMoveTo(entity: CyEntity, target: UnsafePointer<Float>?) throws {
        try interface.navAgentMoveTo(engine: raw, entity: entity, target: target)
    }

    @inlinable
    public func navAgentStop(entity: CyEntity) throws {
        try interface.navAgentStop(engine: raw, entity: entity)
    }

    @inlinable
    public func navAgentState(entity: CyEntity, into: UnsafeMutablePointer<CyNavAgentState>?) throws {
        try interface.navAgentState(engine: raw, entity: entity, into: into)
    }

    @inlinable
    public func audioFindCue(name: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyAudioCue>?) throws {
        try interface.audioFindCue(engine: raw, name: name, into: into)
    }

    @inlinable
    public func audioPlay(play: UnsafePointer<CyAudioPlay>?, voice: UnsafeMutablePointer<CyAudioVoice>?) throws {
        try interface.audioPlay(engine: raw, play: play, voice: voice)
    }

    @inlinable
    public func audioStop(voice: CyAudioVoice, fadeOut: Float) throws {
        try interface.audioStop(engine: raw, voice: voice, fadeOut: fadeOut)
    }

    @inlinable
    public func audioVoicePlaying(voice: CyAudioVoice) -> Bool {
        interface.audioVoicePlaying(engine: raw, voice: voice)
    }

    @inlinable
    public func audioFindBus(name: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyAudioBus>?) throws {
        try interface.audioFindBus(engine: raw, name: name, into: into)
    }

    @inlinable
    public func audioSetBusVolume(bus: CyAudioBus, volume: Float, fade: Float) throws {
        try interface.audioSetBusVolume(engine: raw, bus: bus, volume: volume, fade: fade)
    }

    @inlinable
    public func spawnResolve(asset: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyPrefab>?) throws {
        try interface.spawnResolve(engine: raw, asset: asset, into: into)
    }

    @inlinable
    public func spawnInstantiate(prefab: CyPrefab, params: UnsafePointer<CySpawnParams>?, root: UnsafeMutablePointer<CyEntity>?) throws {
        try interface.spawnInstantiate(engine: raw, prefab: prefab, params: params, root: root)
    }

    @inlinable
    public func spawnInstantiateMany(prefab: CyPrefab, parent: CyEntity, poses: UnsafePointer<CyPose>?, count: UInt32, roots: UnsafeMutablePointer<CyEntity>?) throws {
        try interface.spawnInstantiateMany(engine: raw, prefab: prefab, parent: parent, poses: poses, count: count, roots: roots)
    }

    @inlinable
    public func spawnDestroy(root: CyEntity) throws {
        try interface.spawnDestroy(engine: raw, root: root)
    }

    @inlinable
    public func vfxEffectParameterSet(entity: CyEntity, emitter: UnsafePointer<CChar>?, parameter: UnsafePointer<CChar>?, value: UnsafePointer<CyVar>?) throws {
        try interface.vfxEffectParameterSet(engine: raw, entity: entity, emitter: emitter, parameter: parameter, value: value)
    }

    @inlinable
    public func vfxEffectParameterGet(entity: CyEntity, emitter: UnsafePointer<CChar>?, parameter: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyVar>?) throws {
        try interface.vfxEffectParameterGet(engine: raw, entity: entity, emitter: emitter, parameter: parameter, into: into)
    }

    @inlinable
    public func registerSystem(desc: UnsafePointer<CySystemDesc>?) throws {
        try interface.registerSystem(engine: raw, desc: desc)
    }

    @inlinable
    public func nodeFind(from: CyEntity, path: UnsafePointer<CChar>?, into: UnsafeMutablePointer<CyEntity>?) throws {
        try interface.nodeFind(engine: raw, from: from, path: path, into: into)
    }

    @inlinable
    public func physicsApplyForce(entity: CyEntity, force: UnsafePointer<Float>?) throws {
        try interface.physicsApplyForce(engine: raw, entity: entity, force: force)
    }

    @inlinable
    public func physicsApplyImpulse(entity: CyEntity, impulse: UnsafePointer<Float>?, point: UnsafePointer<Float>?) throws {
        try interface.physicsApplyImpulse(engine: raw, entity: entity, impulse: impulse, point: point)
    }

    @inlinable
    public func physicsApplyTorque(entity: CyEntity, torque: UnsafePointer<Float>?) throws {
        try interface.physicsApplyTorque(engine: raw, entity: entity, torque: torque)
    }

    @inlinable
    public func physicsSetVelocity(entity: CyEntity, linear: UnsafePointer<Float>?, angular: UnsafePointer<Float>?) throws {
        try interface.physicsSetVelocity(engine: raw, entity: entity, linear: linear, angular: angular)
    }

    @inlinable
    public func physicsGetVelocity(entity: CyEntity, linear: UnsafeMutablePointer<Float>?, angular: UnsafeMutablePointer<Float>?) throws {
        try interface.physicsGetVelocity(engine: raw, entity: entity, linear: linear, angular: angular)
    }

    @inlinable
    public func characterCreate(entity: CyEntity, desc: UnsafePointer<CyCharacterDesc>?) throws {
        try interface.characterCreate(engine: raw, entity: entity, desc: desc)
    }

    @inlinable
    public func characterDestroy(entity: CyEntity) throws {
        try interface.characterDestroy(engine: raw, entity: entity)
    }

    @inlinable
    public func characterMove(entity: CyEntity, input: UnsafePointer<CyCharacterInput>?) throws {
        try interface.characterMove(engine: raw, entity: entity, input: input)
    }

    @inlinable
    public func characterState(entity: CyEntity, into: UnsafeMutablePointer<CyCharacterState>?) throws {
        try interface.characterState(engine: raw, entity: entity, into: into)
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

    @inlinable
    public func componentCount() -> UInt32 {
        interface.worldComponentCount(world: raw)
    }

    @inlinable
    public func componentInfo(component: CyComponentTypeId, into: UnsafeMutablePointer<CyComponentInfo>?) throws {
        try interface.worldComponentInfo(world: raw, component: component, into: into)
    }

    @inlinable
    public func componentField(component: CyComponentTypeId, field: UInt32, into: UnsafeMutablePointer<CyFieldDesc>?) throws {
        try interface.worldComponentField(world: raw, component: component, field: field, into: into)
    }

    @inlinable
    public func parent(entity: CyEntity) -> CyEntity {
        interface.worldParent(world: raw, entity: entity)
    }

    @inlinable
    public func setParent(child: CyEntity, parent: CyEntity) throws {
        try interface.worldSetParent(world: raw, child: child, parent: parent)
    }

    @inlinable
    public func childCount(entity: CyEntity) -> UInt32 {
        interface.worldChildCount(world: raw, entity: entity)
    }

    @inlinable
    public func child(entity: CyEntity, index: UInt32) -> CyEntity {
        interface.worldChild(world: raw, entity: entity, index: index)
    }

    @inlinable
    public func chunks(component: CyComponentTypeId, into: UnsafeMutablePointer<CyChunk>?, capacity: UInt32, count: UnsafeMutablePointer<UInt32>?) throws {
        try interface.worldChunks(world: raw, component: component, into: into, capacity: capacity, count: count)
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
